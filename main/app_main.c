#include "app.h"
#include "appcfg.h"
#include "common.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "mqtt_app.h"
#include "network.h"
#include "nvs_flash.h"
#include "ota.h"
#include "periph.h"
#include "sensor.h"
#include "token.h"
#include "wifi.h"

static const char *TAG = "app_main";

/* 全局状态 */
esp_mqtt_client_handle_t g_mqttClient = NULL;
volatile bool g_isMQTTConnected = false;
volatile bool g_authSuccess = false;
QueueHandle_t g_commandQueue = NULL;
EventGroupHandle_t g_wifiEventGroup = NULL;
uint32_t g_reportIntervalMs = SEND_INTERVAL;

/* 上电时 GPIO 被拉低则擦 NVS 重启,先于 nvs_flash_init 执行 */
static void factory_reset_check(void) {
  gpio_config_t cfg = {
      .pin_bit_mask = 1ULL << FACTORY_RESET_GPIO,
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  ESP_ERROR_CHECK(gpio_config(&cfg));

  vTaskDelay(pdMS_TO_TICKS(10)); /* 等上拉建立,滤掉上电抖动 */

  if (gpio_get_level(FACTORY_RESET_GPIO) != 0)
    return;

  ESP_LOGW(TAG,
           "Factory reset: GPIO%d pulled low, erasing all saved parameters",
           FACTORY_RESET_GPIO);
  ESP_ERROR_CHECK(nvs_flash_erase());
  ESP_LOGW(TAG, "NVS erased, restarting to provisioning mode");
  esp_restart();
}

/* TLS 握手耗 CPU,放独立任务避免饿死 IDLE 触发 WDT */
static void networkInitTask(void *pv) {
  (void)pv;
  ESP_LOGI(TAG, "[Core %d] Network init task started", (int)xPortGetCoreID());

  g_authSuccess = authbydeviceid();
  if (g_authSuccess)
    syncPendingCommands();

  xEventGroupSetBits(g_wifiEventGroup, NETWORK_INIT_DONE_BIT);
  ESP_LOGI(TAG, "Network init task done (auth=%s)",
           g_authSuccess ? "ok" : "fail");
  vTaskDelete(NULL);
}

/* 重启要重新关联 WiFi 和 TLS 握手,指数退避 5s→2min。
 * esp_restart 会重载 .rtc.data,必须用 RTC_NOINIT + magic 才能跨重启保留。 */
#define RESTART_BACKOFF_MAGIC 0x5A17B00FU
#define RESTART_BACKOFF_MIN_MS 5000U
#define RESTART_BACKOFF_MAX_MS 120000U
RTC_NOINIT_ATTR static uint32_t s_restartBackoffMs;
RTC_NOINIT_ATTR static uint32_t s_restartBackoffMagic;

static uint32_t restart_backoff_ms(void) {
  if (s_restartBackoffMagic != RESTART_BACKOFF_MAGIC ||
      s_restartBackoffMs < RESTART_BACKOFF_MIN_MS ||
      s_restartBackoffMs > RESTART_BACKOFF_MAX_MS)
    s_restartBackoffMs = RESTART_BACKOFF_MIN_MS;
  return s_restartBackoffMs;
}

static void backoff_restart(const char *why) {
  const uint32_t delay_ms = restart_backoff_ms();
  ESP_LOGE("main", "%s: restarting in %u s", why,
           (unsigned)(delay_ms / 1000U));
  vTaskDelay(pdMS_TO_TICKS(delay_ms));
  s_restartBackoffMagic = RESTART_BACKOFF_MAGIC;
  s_restartBackoffMs = delay_ms * 2U;
  if (s_restartBackoffMs > RESTART_BACKOFF_MAX_MS)
    s_restartBackoffMs = RESTART_BACKOFF_MAX_MS;
  esp_restart();
}

/* 恢复在独立任务里跑：mqtt_app_destroy 最长阻塞 30s,TLS 握手也耗 CPU。
 * full_reauth=false 复用 token,true 用引导密钥重新认证。
 * 顺序：毁旧客户端 → sync → 重建。 */
static void connectionRecoveryTask(void *pv) {
  const bool full_reauth = (pv != NULL);

  if (g_mqttClient)
    mqtt_app_destroy(g_mqttClient); /* 内部先摘全局句柄再阻塞 stop */

  if (full_reauth) {
    token_clear();
    g_authSuccess = authbydeviceid();
    if (!g_authSuccess)
      backoff_restart("Re-auth failed");
  }

  syncPendingCommands();

  g_mqttClient = mqtt_app_create();
  if (g_mqttClient == NULL)
    backoff_restart("Failed to re-create MQTT client");
  mqtt_app_start(g_mqttClient);

  xEventGroupSetBits(g_wifiEventGroup, RECOVERY_DONE_BIT);
  ESP_LOGI(TAG, "Connection recovered (%s path)",
           full_reauth ? "full re-auth" : "token reuse");
  vTaskDelete(NULL);
}

/* 栈 10KB：要跑 TLS 和 sync；120s 超时则退避重启 */
static void run_recovery(bool full_reauth) {
  xEventGroupClearBits(g_wifiEventGroup, RECOVERY_DONE_BIT);
  if (xTaskCreatePinnedToCore(connectionRecoveryTask, "Recover", 10240,
                              full_reauth ? (void *)1 : NULL, 2, NULL,
                              0) != pdPASS)
    backoff_restart("Failed to spawn recovery task");

  EventBits_t rb = xEventGroupWaitBits(g_wifiEventGroup, RECOVERY_DONE_BIT,
                                       pdTRUE, pdTRUE, pdMS_TO_TICKS(120000));
  if (!(rb & RECOVERY_DONE_BIT))
    backoff_restart("Connection recovery stuck");

  s_restartBackoffMs = RESTART_BACKOFF_MIN_MS; /* 恢复成功,退避归零 */
  s_restartBackoffMagic = RESTART_BACKOFF_MAGIC;
}

void app_main(void) {
  ESP_LOGI(TAG, "Device starting...");

  /* --- 工厂复位 --- */
  factory_reset_check();

  /* --- NVS --- */
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  /* DFS 160↔80 MHz + 空闲 light sleep；USB-JTAG 连接时 sdkconfig 自动禁用睡眠 */
  esp_pm_config_t pm_config = {
      .max_freq_mhz = 160,
      .min_freq_mhz = 80,
      .light_sleep_enable = (PWR_SAVE_ENABLE == 1),
  };
  ESP_ERROR_CHECK(esp_pm_configure(&pm_config));

  /* 配置状态在 MQTT 事件前就绪 */
  appcfg_init();

  /* 屏蔽 PHY 日志,避免 IDF 6.0 printf 锁崩溃 */
  esp_log_level_set("phy", ESP_LOG_NONE);
  esp_log_level_set("wifi", ESP_LOG_WARN);

  /* --- WiFi --- */
  if (!wifi_init_sta())
    backoff_restart("WiFi connection failed");

  /* 独立 OTA：固定 URL 检查一次，有新固件则写入并重启 */
  ota_check_on_boot();

  /* 必须在 networkInitTask 前创建,syncPendingCommands 会往里入队 */
  g_commandQueue = xQueueCreate(20, sizeof(CommandMsg));
  if (g_commandQueue == NULL)
    backoff_restart("Failed to create command queue");

  /* spawn 前清位,等待时 clear-on-exit */
  xEventGroupClearBits(g_wifiEventGroup, NETWORK_INIT_DONE_BIT);
  if (xTaskCreatePinnedToCore(networkInitTask, "NetInit", 8192, NULL, 2, NULL,
                              0) != pdPASS)
    backoff_restart("Failed to create net init task");
  EventBits_t netBits =
      xEventGroupWaitBits(g_wifiEventGroup, NETWORK_INIT_DONE_BIT, pdTRUE,
                          pdTRUE, pdMS_TO_TICKS(30000));
  if (!(netBits & NETWORK_INIT_DONE_BIT))
    backoff_restart("Network init timeout");
  if (!g_authSuccess)
    backoff_restart("Authentication failed");

  /* --- Temperature sensor --- */
  if (!sensor_init()) {
    ESP_LOGW(TAG, "Temperature sensor init failed, continuing anyway...");
  }

  /* --- Peripheral bus: register drivers --- */
  periph_bus_init();
  /* 占位,防止云端把复位脚当普通 IO 分配 */
  if (!periph_pin_claim(FACTORY_RESET_GPIO, "factory_reset"))
    ESP_LOGW(TAG, "GPIO%d already claimed, factory reset pin not reserved",
             FACTORY_RESET_GPIO);
  /* 从 NVS 快照恢复执行器与上报周期 */
  appcfg_replay();

  if (g_authSuccess)
    reportPanelInfo();
  /* --- MQTT client (WSS) --- */
  g_mqttClient = mqtt_app_create();
  if (g_mqttClient == NULL)
    backoff_restart("Failed to create MQTT client");
  mqtt_app_start(g_mqttClient);

  /* --- Tasks --- */
  xTaskCreatePinnedToCore(sensorReportTask, "SensorReportTask", 10240, NULL, 1,
                          NULL, 0);

  xTaskCreatePinnedToCore(cmdProcessTask, "CmdProcessTask", 8192, NULL, 2, NULL,
                          1);

  ESP_LOGI(TAG, "System init complete");

  /* 主循环监督 WiFi/MQTT：WiFi 掉线、CONNACK 拒绝、或 WiFi 在线但 MQTT
   * 断连超时,都进恢复流程；进入前清掉开机残留事件位。 */
  xEventGroupClearBits(g_wifiEventGroup, WIFI_DISCONNECTED_BIT |
                                             MQTT_AUTH_REFUSED_BIT |
                                             RECOVERY_DONE_BIT);

  TickType_t mqttDownSince =
      0; /* 0 = 未计时 */
  uint32_t mqttStuckWaitMs =
      MQTT_STUCK_DEADLINE_MS; /* 断连容忍时长,连续失败翻倍 */
  for (;;) {
    EventBits_t bits = xEventGroupWaitBits(
        g_wifiEventGroup, WIFI_DISCONNECTED_BIT | MQTT_AUTH_REFUSED_BIT, pdTRUE,
        pdFALSE, pdMS_TO_TICKS(MQTT_SUPERVISE_PERIOD_MS));

    /* 同时置位时按全量重认证处理,省一次 TLS */
    bool full_reauth = (bits & MQTT_AUTH_REFUSED_BIT) != 0;
    bool wifi_was_down = (bits & WIFI_DISCONNECTED_BIT) != 0;
    bool supervise_force = false;

    if (wifi_was_down) {
      ESP_LOGW("main", "WiFi disconnected, waiting for reconnect...");
      g_isMQTTConnected = false;
      mqttDownSince =
          0; /* 由下方 WiFi 恢复流程接管 */
      mqttStuckWaitMs =
          MQTT_STUCK_DEADLINE_MS; /* 重建连接后归位 */

      /* esp_wifi 会自动重连；最多等 4×30s */
      bool reconnected = false;
      for (int i = 0; i < 4 && !reconnected; i++) {
        EventBits_t cb =
            xEventGroupWaitBits(g_wifiEventGroup, WIFI_CONNECTED_BIT, pdFALSE,
                                pdFALSE, pdMS_TO_TICKS(30000));
        reconnected = (cb & WIFI_CONNECTED_BIT) != 0;
        if (!reconnected)
          ESP_LOGW("main", "WiFi still down after %d s, keep waiting",
                   (i + 1) * 30);
      }
      if (!reconnected)
        backoff_restart("WiFi reconnection timeout");
    } else if (!full_reauth) {
      /* 仅 WiFi 在线且无认证拒绝时累计断连时长 */
      if ((xEventGroupGetBits(g_wifiEventGroup) & WIFI_CONNECTED_BIT) &&
          !g_isMQTTConnected) {
        if (mqttDownSince == 0)
          mqttDownSince = xTaskGetTickCount();
        else if (xTaskGetTickCount() - mqttDownSince >=
                 pdMS_TO_TICKS(mqttStuckWaitMs)) {
          ESP_LOGW("main",
                   "MQTT down for %u s while WiFi is up, forcing "
                   "supervised recovery (token reuse)",
                   (unsigned)(mqttStuckWaitMs / 1000));
          mqttDownSince = 0;
          supervise_force = true;
          /* 仍失败则翻倍,连上后归位 */
          mqttStuckWaitMs *= 2U;
          if (mqttStuckWaitMs > MQTT_SUPERVISE_MAX_MS)
            mqttStuckWaitMs = MQTT_SUPERVISE_MAX_MS;
        }
      } else {
        mqttDownSince = 0; /* 已连接或 WiFi 不在线,清零 */
        mqttStuckWaitMs = MQTT_STUCK_DEADLINE_MS; /* 连接恢复,归位 */
      }
    }

    /* 只在恢复成功后归零,否则失败重启永远只等 5s */
    if (wifi_was_down || full_reauth || supervise_force) {
      mqttDownSince = 0; /* 重建连接,重新计时 */
      run_recovery(full_reauth);
    }
  }
}
