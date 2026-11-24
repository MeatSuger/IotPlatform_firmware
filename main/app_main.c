#include <stdlib.h>

#include "app.h"
#include "appcfg.h"
#include "cJSON.h"
#include "common.h"
#include "display.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_system.h"
#include "mqtt_app.h"
#include "network.h"
#include "nvs_flash.h"
#include "ota.h"
#include "periph.h"
#include "sensor.h"
#include "token.h"
#include "wifi.h"
#include "psa/crypto.h"

static const char *TAG = "app_main";

/* 全局状态 */
esp_mqtt_client_handle_t g_mqttClient = NULL;
volatile bool g_isMQTTConnected = false;
volatile bool g_authSuccess = false;
QueueHandle_t g_commandQueue = NULL;
EventGroupHandle_t g_wifiEventGroup = NULL;
TaskHandle_t g_reportTask = NULL;
uint32_t g_reportIntervalMs = SEND_INTERVAL;

/* 常驻任务规格(栈字节 / 优先级 / 核)：全部只创建一次,运行期不再建/删任务 */
#define NET_TASK_STACK 10240
#define NET_TASK_PRIO 2
#define SUPERVISE_TASK_STACK 3072
#define SUPERVISE_TASK_PRIO 1

/* 网络任务请求：所有建连/重连动作都由这一个常驻任务执行 */
typedef enum {
  NET_REQ_START = 0, /* 面板上报 + 首次建连(外设重放完成后由 app_main 发出) */
  NET_REQ_RECOVER,   /* 复用 Token 重建 MQTT */
  NET_REQ_REAUTH,    /* 清 Token,用引导密钥重新认证 */
} net_req_t;

static QueueHandle_t s_netReqQ;

/* 等 app_main 完成外设重放/面板探测的最长时间 */
#define NET_START_WAIT_MS 60000U

/* 面板引脚是板载固定接线：开机即占位,防止云端把它分配给普通执行器 */
static void panel_pins_claim(void) {
  static const int pins[] = {DISPLAY_PIN_MOSI, DISPLAY_PIN_SCK,
                             DISPLAY_PIN_CS,   DISPLAY_PIN_DC,
                             DISPLAY_PIN_RST,  DISPLAY_PIN_BUSY};
  for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
    if (!periph_pin_claim(pins[i], "panel"))
      ESP_LOGW(TAG, "GPIO%d already claimed, panel pin not reserved", pins[i]);
  }
}

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
  ESP_LOGE(TAG, "%s: restarting in %u s", why, (unsigned)(delay_ms / 1000U));
  vTaskDelay(pdMS_TO_TICKS(delay_ms));
  s_restartBackoffMagic = RESTART_BACKOFF_MAGIC;
  s_restartBackoffMs = delay_ms * 2U;
  if (s_restartBackoffMs > RESTART_BACKOFF_MAX_MS)
    s_restartBackoffMs = RESTART_BACKOFF_MAX_MS;
  esp_restart();
}

/* 面板信息复用现有 telemetry 通道：发一条不带 sensors 的 telemetry 消息。
 * 后端在 DeviceStatusDTO 里解析 panel；不再单独开 HTTPS，也不新增主题。 */
static void report_panel_mqtt(void) {
  const char *chip = display_chip_id();
  const display_info_t *info = display_info();
  if (chip == NULL || info == NULL) {
    ESP_LOGD(TAG, "Panel not initialized, skip panel report");
    return;
  }

  cJSON *body = cJSON_CreateObject();
  if (body == NULL) {
    return;
  }
  cJSON *panel = cJSON_AddObjectToObject(body, "panel");
  if (panel == NULL) {
    cJSON_Delete(body);
    return;
  }
  cJSON_AddStringToObject(panel, "chip", chip);
  cJSON_AddNumberToObject(panel, "width", info->width);
  cJSON_AddNumberToObject(panel, "height", info->height);

  char *payload = cJSON_PrintUnformatted(body);
  cJSON_Delete(body);
  if (payload == NULL) {
    return;
  }

  int msg_id = mqtt_publish_qos(MQTT_TOPIC_DATA, payload, strlen(payload), 1);
  if (msg_id < 0) {
    ESP_LOGW(TAG, "Panel telemetry report failed");
  } else {
    ESP_LOGI(TAG, "Panel reported via telemetry: %s %ux%u", chip, info->width,
             info->height);
  }
  free(payload);
}

/* 认证(可选)→ 拉取离线命令 → 重建 MQTT → 面板上报。
 * full_reauth=false 复用 Token,true 用引导密钥重新认证。
 * 只在网络任务里调用,句柄生命周期永远由同一任务持有。 */
static void net_connect(bool full_reauth) {
  if (g_mqttClient)
    mqtt_app_destroy(g_mqttClient); /* 内部先摘全局句柄再阻塞 stop */

  if (full_reauth) {
    token_clear();
    g_authSuccess = authbydeviceid();
    if (!g_authSuccess)
      backoff_restart("Re-auth failed");
  }

  syncPendingCommands();

  /* 新客户端尚未连上，先清标志，避免旧连接状态让面板上报抢跑 */
  g_isMQTTConnected = false;

  esp_mqtt_client_handle_t client = mqtt_app_create();
  if (client == NULL)
    backoff_restart("Failed to create MQTT client");
  mqtt_app_start(client);

  /* 面板信息也走 MQTT：等连接成功后再发；未连上就跳过，下次重连再报。 */
  for (int i = 0; i < 100 && !g_isMQTTConnected; i++)
    vTaskDelay(pdMS_TO_TICKS(100));

  if (!g_isMQTTConnected) {
    ESP_LOGW(TAG, "MQTT not connected, skip panel report");
    return;
  }

  /* 全新设备没有 epd 配置,appcfg_replay 不会探测面板,这里兜底初始化,
   * 否则拿不到芯片型号；上报失败留到下次重连再试。 */
  ESP_LOGI(TAG, "panel report, free heap %u",
           (unsigned)esp_get_free_heap_size());
  if (display_chip_id() == NULL)
    display_init();
  report_panel_mqtt();
}

/* 常驻网络任务(栈 10KB：TLS 握手 + 命令同步都必须在自己栈上跑)：
 * 开机认证一次,等 app_main 放行后建连；之后只按请求重连。 */
static void networkTask(void *pv) {
  (void)pv;
  ESP_LOGI(TAG, "[Core %d] Network task started", (int)xPortGetCoreID());

  g_authSuccess = authbydeviceid();
  xEventGroupSetBits(g_wifiEventGroup, NETWORK_INIT_DONE_BIT);
  ESP_LOGI(TAG, "Network init done (auth=%s)", g_authSuccess ? "ok" : "fail");
  if (!g_authSuccess) {
    vTaskDelete(NULL); /* app_main 看到失败会退避重启 */
    return;
  }

  net_req_t req;
  if (xQueueReceive(s_netReqQ, &req, pdMS_TO_TICKS(NET_START_WAIT_MS)) !=
      pdTRUE)
    backoff_restart("Network start timeout");

  net_connect(false);

  for (;;) {
    if (xQueueReceive(s_netReqQ, &req, portMAX_DELAY) != pdTRUE)
      continue;

    net_connect(req == NET_REQ_REAUTH);
    xEventGroupSetBits(g_wifiEventGroup, RECOVERY_DONE_BIT);
    ESP_LOGI(TAG, "Connection recovered (%s path)",
             req == NET_REQ_REAUTH ? "full re-auth" : "token reuse");
  }
}

/* 请求一次重连并等它完成；网络任务不响应就退避重启 */
static void request_recovery(bool full_reauth) {
  xEventGroupClearBits(g_wifiEventGroup, RECOVERY_DONE_BIT);

  const net_req_t req = full_reauth ? NET_REQ_REAUTH : NET_REQ_RECOVER;
  if (xQueueSend(s_netReqQ, &req, pdMS_TO_TICKS(5000)) != pdTRUE)
    backoff_restart("Failed to queue recovery");

  EventBits_t rb = xEventGroupWaitBits(g_wifiEventGroup, RECOVERY_DONE_BIT,
                                       pdTRUE, pdTRUE, pdMS_TO_TICKS(120000));
  if (!(rb & RECOVERY_DONE_BIT))
    backoff_restart("Connection recovery stuck");

  s_restartBackoffMs = RESTART_BACKOFF_MIN_MS; /* 恢复成功,退避归零 */
  s_restartBackoffMagic = RESTART_BACKOFF_MAGIC;
}

/* 监督 WiFi/MQTT：WiFi 掉线、CONNACK 拒绝、或 WiFi 在线但 MQTT 断连超时,
 * 都请求网络任务重连。 */
static void superviseTask(void *pv) {
  (void)pv;
  ESP_LOGI(TAG, "[Core %d] Supervisor task started", (int)xPortGetCoreID());

  /* 进入前清掉开机残留事件位 */
  xEventGroupClearBits(g_wifiEventGroup, WIFI_DISCONNECTED_BIT |
                                             MQTT_AUTH_REFUSED_BIT |
                                             RECOVERY_DONE_BIT);

  TickType_t mqttDownSince = 0; /* 0 = 未计时 */
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
      ESP_LOGW(TAG, "WiFi disconnected, waiting for reconnect...");
      g_isMQTTConnected = false;
      mqttDownSince = 0;                        /* 由下方 WiFi 恢复流程接管 */
      mqttStuckWaitMs = MQTT_STUCK_DEADLINE_MS; /* 重建连接后归位 */

      /* esp_wifi 会自动重连；最多等 4×30s */
      bool reconnected = false;
      for (int i = 0; i < 4 && !reconnected; i++) {
        EventBits_t cb =
            xEventGroupWaitBits(g_wifiEventGroup, WIFI_CONNECTED_BIT, pdFALSE,
                                pdFALSE, pdMS_TO_TICKS(30000));
        reconnected = (cb & WIFI_CONNECTED_BIT) != 0;
        if (!reconnected)
          ESP_LOGW(TAG, "WiFi still down after %d s, keep waiting",
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
          ESP_LOGW(TAG,
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
      request_recovery(full_reauth);
    }
  }
}

void app_main(void) {
  ESP_LOGI(TAG, "Device starting...");
  psa_status_t status = psa_crypto_init();
  if (status != PSA_SUCCESS) {
    ESP_LOGI(TAG, "TLS init failed (PSA error %d), aborting", (int)status);
  }
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
  ESP_LOGI(TAG, "free heap after NVS: %u", (unsigned)esp_get_free_heap_size());

  /* DFS 160↔80 MHz + 空闲 light sleep；USB-JTAG 连接时 sdkconfig 自动禁用睡眠
   */
  esp_pm_config_t pm_config = {
      .max_freq_mhz = 160,
      .min_freq_mhz = 80,
      .light_sleep_enable = (PWR_SAVE_ENABLE == 1),
  };
  ESP_ERROR_CHECK(esp_pm_configure(&pm_config));

  /* 配置状态在 MQTT 事件前就绪 */
  appcfg_init();

  /* 屏蔽 PHY 日志,避免 IDF 6.0 printf 锁崩溃；mbedTLS 只保留告警/错误 */
  esp_log_level_set("phy", ESP_LOG_NONE);
  esp_log_level_set("wifi", ESP_LOG_WARN);
  esp_log_level_set("mbedtls", ESP_LOG_WARN);

  /* --- WiFi --- */
  if (!wifi_init_sta())
    backoff_restart("WiFi connection failed");
  ESP_LOGI(TAG, "free heap after WiFi: %u", (unsigned)esp_get_free_heap_size());

  /* 独立 OTA：固定 URL 检查一次，有新固件则写入并重启 */
  ota_check_on_boot();

  /* 必须在网络任务前创建,syncPendingCommands 会往里入队 */
  g_commandQueue = xQueueCreate(20, sizeof(CommandMsg));
  if (g_commandQueue == NULL)
    backoff_restart("Failed to create command queue");

  s_netReqQ = xQueueCreate(2, sizeof(net_req_t));
  if (s_netReqQ == NULL)
    backoff_restart("Failed to create network request queue");

  /* 认证先跑(独立任务栈放得下 TLS),通过后 app_main 才继续初始化外设 */
  xEventGroupClearBits(g_wifiEventGroup, NETWORK_INIT_DONE_BIT);
  if (xTaskCreatePinnedToCore(networkTask, "net", NET_TASK_STACK, NULL,
                              NET_TASK_PRIO, NULL, 0) != pdPASS)
    backoff_restart("Failed to create network task");

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
  panel_pins_claim();
  /* 从 NVS 快照恢复执行器与上报周期 */
  appcfg_replay();

  /* 放行网络任务：面板上报 + MQTT 建连 */
  const net_req_t start = NET_REQ_START;
  if (xQueueSend(s_netReqQ, &start, pdMS_TO_TICKS(5000)) != pdTRUE)
    backoff_restart("Failed to start network task");

  /* --- 常驻任务 --- */
  if (xTaskCreatePinnedToCore(sensorReportTask, "report", 10240, NULL, 1,
                              &g_reportTask, 0) != pdPASS)
    backoff_restart("Failed to create report task");
  if (xTaskCreatePinnedToCore(cmdProcessTask, "cmd", 8192, NULL, 2, NULL, 1) !=
      pdPASS)
    backoff_restart("Failed to create command task");
  if (xTaskCreatePinnedToCore(superviseTask, "sup", SUPERVISE_TASK_STACK, NULL,
                              SUPERVISE_TASK_PRIO, NULL, 0) != pdPASS)
    backoff_restart("Failed to create supervisor task");

  ESP_LOGI(TAG, "System init complete");
  /* 启动装配结束,main 任务退出；所有业务都跑在常驻任务里 */
}
