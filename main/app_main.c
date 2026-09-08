#include "common.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "wifi.h"
#include "mqtt_app.h"
#include "app.h"
#include "network.h"
#include "sensor.h"
#include "token.h"
#include "periph.h"
#include "appcfg.h"
#include "esp_pm.h"

static const char *TAG = "app_main";

/* --------------------------------------------------------------------------
 * Global state
 * -------------------------------------------------------------------------- */
esp_mqtt_client_handle_t g_mqttClient = NULL;
volatile bool g_isMQTTConnected = false;
volatile bool g_authSuccess = false;
QueueHandle_t g_commandQueue = NULL;
EventGroupHandle_t g_wifiEventGroup = NULL;
uint32_t g_reportIntervalMs = SEND_INTERVAL;

/* --------------------------------------------------------------------------
 * networkInitTask — runs auth + sync in a dedicated task so that the
 * blocking TLS handshake (ECDSA on ESP32 takes 5–8 s) does not starve
 * the idle task and trigger the task watchdog.
 * -------------------------------------------------------------------------- */
static void networkInitTask(void *pv)
{
    (void)pv;
    ESP_LOGI(TAG, "[Core %d] Network init task started",
                (int)xPortGetCoreID());

    g_authSuccess = authbydeviceid();
    if (g_authSuccess)
        syncPendingCommands();

    xEventGroupSetBits(g_wifiEventGroup, NETWORK_INIT_DONE_BIT);
    ESP_LOGI(TAG, "Network init task done (auth=%s)",
                  g_authSuccess ? "ok" : "fail");
    vTaskDelete(NULL);
}

/* --------------------------------------------------------------------------
 * 异常重启退避
 *
 * 每次重启 = 完整 boot + WiFi 关联 + TLS 握手，是整机最耗电路径；服务器/网络
 * 故障时若立即重启会形成密集重试（boot loop）。指数退避 5s→2min 封顶，
 * 恢复正常后置回 5s（见主循环）。
 * -------------------------------------------------------------------------- */
static uint32_t s_restartBackoffMs = 5000U;
static void backoff_restart(const char *why)
{
    ESP_LOGE("main", "%s: restarting in %u s", why,
             (unsigned)(s_restartBackoffMs / 1000U));
    vTaskDelay(pdMS_TO_TICKS(s_restartBackoffMs));
    s_restartBackoffMs *= 2U;
    if (s_restartBackoffMs > 120000U)
        s_restartBackoffMs = 120000U;
    esp_restart();
}

/* --------------------------------------------------------------------------
 * connectionRecoveryTask — 连接恢复统一入口（全程在专用任务里跑，主任务只等待）。
 *
 * 两种模式（pv != NULL = 全量重认证）：
 *   A. 快路径（WiFi 重连后）：设备在线期间的周期上报会刷新 token，重连时
 *      token 大概率仍有效，复用现有 token 重建客户端，省一次 5-8s TLS 握手；
 *      若 broker 拒绝（CONNACK refused）→ 事件位再次置位 → 下轮全量恢复，自动收敛。
 *   B. 全量重认证（token 确认失效）：清 token → DeviceIDAuth 重新取 → 重建。
 *
 * 为什么整体必须离开主任务：
 *   - mqtt_app_destroy 的 stop 在 WAIT_RECONNECT 态最长可阻塞近
 *     reconnect_timeout/2（约 30s），只能在恢复任务自身上下文承受；
 *   - 认证/sync 的 TLS 握手 CPU-bound 5-8s，在主任务跑会饿死 IDLE 触发 5s
 *     task WDT panic 复位，且复位不带退避 → boot+关联+TLS 全握手密集重刷。
 *
 * 顺序即正确性：先毁旧客户端（置 NULL 防 use-after-free；同时掐灭失效 token
 * 的自动重试风暴，毁/建窗口内不再产生新事件位），后 sync 兜底，最后重建/
 * start。离线命令由 sync 拉取即消费入队，与 retained config 快照重投互不冲突。
 * -------------------------------------------------------------------------- */
static void connectionRecoveryTask(void *pv)
{
    const bool full_reauth = (pv != NULL);

    /* 1) 先毁旧客户端 */
    if (g_mqttClient)
    {
        mqtt_app_destroy(g_mqttClient);
        g_mqttClient = NULL; /* prevent use-after-free from other tasks */
    }

    /* 2) 全量模式：重新取 Token */
    if (full_reauth)
    {
        token_clear();
        g_authSuccess = authbydeviceid();
        if (!g_authSuccess)
            backoff_restart("Re-auth failed");
    }

    /* 3) 离线命令兜底 */
    syncPendingCommands();

    /* 4) 用当前 token 重建并启动客户端 */
    g_mqttClient = mqtt_app_create();
    if (g_mqttClient == NULL)
        backoff_restart("Failed to re-create MQTT client");
    mqtt_app_start(g_mqttClient);

    xEventGroupSetBits(g_wifiEventGroup, RECOVERY_DONE_BIT);
    ESP_LOGI(TAG, "Connection recovered (%s path)",
                  full_reauth ? "full re-auth" : "token reuse");
    vTaskDelete(NULL);
}

/* 启动一次连接恢复并等待完成。栈 10240（比 NetInit 的 8192 大：本任务同时
 * 承载 TLS 握手与 sync）；120s 上界 = 毁(≤30s)+认证(≤30s)+sync(≤15s)+重建+裕量，
 * 超时视为卡死兜底退避重启。退避计数仅在确认恢复成功后归零。 */
static void run_recovery(bool full_reauth)
{
    xEventGroupClearBits(g_wifiEventGroup, RECOVERY_DONE_BIT);
    if (xTaskCreatePinnedToCore(connectionRecoveryTask, "Recover", 10240,
                                full_reauth ? (void *)1 : NULL, 2, NULL, 0) != pdPASS)
        backoff_restart("Failed to spawn recovery task");

    EventBits_t rb = xEventGroupWaitBits(g_wifiEventGroup, RECOVERY_DONE_BIT,
                                         pdTRUE, pdTRUE,
                                         pdMS_TO_TICKS(120000));
    if (!(rb & RECOVERY_DONE_BIT))
        backoff_restart("Connection recovery stuck");

    s_restartBackoffMs = 5000U; /* 恢复成功，退避归零 */
}

void app_main(void)
{
    ESP_LOGI(TAG, "Device starting...");

    /* --- NVS (required by WiFi) --- */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* PM：DFS 160↔80 MHz（S3 上 APB 恒 80 MHz，UART/RMT 等外设时序不受降频影响；
       TLS/命令处理等忙时驱动 PM 锁自动跑 160 MHz）+ 空闲自动 light sleep（双核，
       配合 WiFi modem-sleep 按 DTIM 唤醒，MQTT 长连接保持）。USB-SJC 副控制台与
       light sleep 互斥：sdkconfig 已开 USJ_NO_AUTO_LS_ON_CONNECTION，PC 插着 USB
       时自动不睡（端口稳定），拔除后省电全量生效。 */
    esp_pm_config_t pm_config = {
        .max_freq_mhz = 160,
        .min_freq_mhz = 80,
        .light_sleep_enable = (PWR_SAVE_ENABLE == 1),
    };
    ESP_ERROR_CHECK(esp_pm_configure(&pm_config));

    /* 设备配置持久化状态（NVS）在 MQTT 事件前就绪：版本/载荷/待回执 */
    appcfg_init();

    /* Suppress PHY lib debug output — prevents printf lock crash in IDF 6.0 */
    esp_log_level_set("phy", ESP_LOG_NONE);
    esp_log_level_set("wifi", ESP_LOG_WARN);

    /* --- WiFi --- */
    if (!wifi_init_sta())
        backoff_restart("WiFi connection failed");

    /* --- Command queue ---
     * 必须在 networkInitTask 之前创建：该任务里的 syncPendingCommands 会向队列
     * 入队离线命令（队列为 NULL 将触发断言崩溃）。深度 20：离线期间可积压多条
     * 控制/配置命令，cmdProcessTask 尚未启动时全部暂存于此。 */
    g_commandQueue = xQueueCreate(20, sizeof(CommandMsg));
    if (g_commandQueue == NULL)
        backoff_restart("Failed to create command queue");

    /* --- Authentication + pending-command sync ---
     * 独立任务：ESP32 无硬件加速，TLS ECDSA 握手需 5-8s，在主任务跑会饿死
     * IDLE 触发 5s task WDT。NETWORK_INIT_DONE_BIT 必须 spawn 前清位、等待时
     * clear-on-exit，否则该位自开机起常置，等待会短路空转。 */
    xEventGroupClearBits(g_wifiEventGroup, NETWORK_INIT_DONE_BIT);
    if (xTaskCreatePinnedToCore(networkInitTask, "NetInit", 8192,
                                NULL, 2, NULL, 0) != pdPASS)
        backoff_restart("Failed to create net init task");
    EventBits_t netBits = xEventGroupWaitBits(g_wifiEventGroup,
                                              NETWORK_INIT_DONE_BIT,
                                              pdTRUE, pdTRUE,
                                              pdMS_TO_TICKS(30000));
    if (!(netBits & NETWORK_INIT_DONE_BIT))
        backoff_restart("Network init timeout");
    if (!g_authSuccess)
        backoff_restart("Authentication failed");

    /* --- Temperature sensor --- */
    if (!sensor_init())
    {
        ESP_LOGW(TAG, "Temperature sensor init failed, continuing anyway...");
    }

    /* --- Peripheral bus: register drivers --- */
    periph_bus_init();
    /* 从云端配置快照（NVS）重放执行器定义与上报周期 —— 配置定义唯一真源 */
    appcfg_replay();
    /* --- MQTT client (WSS) --- */
    g_mqttClient = mqtt_app_create();
    if (g_mqttClient == NULL)
        backoff_restart("Failed to create MQTT client");
    mqtt_app_start(g_mqttClient);

    /* --- Tasks --- */
    /* Core 0: periodic sensor report via MQTT */
    xTaskCreatePinnedToCore(sensorReportTask, "SensorReportTask", 10240,
                            NULL, 1, NULL, 0);

    /* Core 1: command processing */
    xTaskCreatePinnedToCore(cmdProcessTask, "CmdProcessTask", 8192,
                            NULL, 2, NULL, 1);

    ESP_LOGI(TAG, "System init complete");

    /* 主线程 = WiFi / MQTT 看门狗：纯事件驱动阻塞等待，稳定期零唤醒，
     * 不打断 light sleep。esp_wifi_connect 自动重连已在 WiFi 事件处理器里。 */
    /* 进入监控前先清开机窗口残留的 latch：防止开机阶段一次瞬时抖动就在首轮对
     * 健康客户端做虚假恢复（多烧一次 TLS）。若此刻真处于掉线态，esp_wifi/esp-mqtt
     * 自动重连与 CONNACK 拒绝路径会再次置位收敛。 */
    xEventGroupClearBits(g_wifiEventGroup,
                         WIFI_DISCONNECTED_BIT | MQTT_AUTH_REFUSED_BIT |
                         RECOVERY_DONE_BIT);

    for (;;)
    {
        EventBits_t bits = xEventGroupWaitBits(g_wifiEventGroup,
                                               WIFI_DISCONNECTED_BIT | MQTT_AUTH_REFUSED_BIT,
                                               pdTRUE, pdFALSE, portMAX_DELAY);

        /* 拒绝位优先：双位同至时一次全量恢复即可。若先走快路径，失效 token
         * 大概率又被拒，白烧 2 次 TLS 握手。 */
        bool full_reauth = (bits & MQTT_AUTH_REFUSED_BIT) != 0;

        if (bits & WIFI_DISCONNECTED_BIT)
        {
            ESP_LOGW("main", "WiFi disconnected, waiting for reconnect...");
            g_isMQTTConnected = false;

            /* esp_wifi 事件处理器已自动重连；最多等 4×30s，仍失败才退避重启 */
            bool reconnected = false;
            for (int i = 0; i < 4 && !reconnected; i++)
            {
                EventBits_t cb = xEventGroupWaitBits(g_wifiEventGroup,
                                                     WIFI_CONNECTED_BIT,
                                                     pdFALSE, pdFALSE,
                                                     pdMS_TO_TICKS(30000));
                reconnected = (cb & WIFI_CONNECTED_BIT) != 0;
                if (!reconnected)
                    ESP_LOGW("main", "WiFi still down after %d s, keep waiting",
                             (i + 1) * 30);
            }
            if (!reconnected)
                backoff_restart("WiFi reconnection timeout");
        }

        /* 退避归零只存在于 run_recovery 成功返回之后：若在恢复动作前归零，
         * 恢复内部失败触发的重启永远只等 5s，风暴兜底形同虚设。 */
        run_recovery(full_reauth);
    }
}
