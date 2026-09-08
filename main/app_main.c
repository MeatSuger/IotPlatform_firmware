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

    /* 主线程 = WiFi / MQTT 看门狗。esp_wifi_connect 自动重连已在 WiFi 事件处理器里；
     * 进入监控前先清开机窗口残留的 latch：防止开机阶段一次瞬时抖动就在首轮对
     * 健康客户端做虚假恢复（多烧一次 TLS）。若此刻真处于掉线态，esp_wifi/esp-mqtt
     * 自动重连与 CONNACK 拒绝路径会再次置位收敛。
     *
     * 三个触发源：
     *   1) WIFI_DISCONNECTED_BIT —— WiFi 掉线：等 WiFi 重连后 token-reuse 恢复
     *   2) MQTT_AUTH_REFUSED_BIT —— broker CONNACK 拒绝：全量重认证恢复
     *   3) 监督超时（新增）      —— WiFi 在线但 MQTT 连续断连超过
     *      MQTT_STUCK_DEADLINE_MS。esp-mqtt 的 auto-reconnect 是库内部静默行为
     *      （reconnect_timeout 周期、无事件上抛、日志在 debug 级）；若其内部重连
     *      因 TCP 半开/socket 陈旧/TLS、DNS 瞬时异常而卡住，WiFi 在线时本固件将
     *      收不到任何事件、永不重连（正是“MQTT 断连后无法重新连接”）。故以
     *      MQTT_SUPERVISE_PERIOD_MS 周期唤醒主循环检查，超时即强制一次 token-reuse
     *      恢复，把重连主动权收回应用层。原“稳定期零唤醒”因此改为低频周期唤醒
     *      （30s 一次，耗电与 DTIM 周期唤醒相比可忽略）。 */
    xEventGroupClearBits(g_wifiEventGroup,
                         WIFI_DISCONNECTED_BIT | MQTT_AUTH_REFUSED_BIT |
                         RECOVERY_DONE_BIT);

    TickType_t mqttDownSince = 0; /* 0 = 未计时；非 0 = “WiFi 在线 && MQTT 断连”计时起点 */
    uint32_t mqttStuckWaitMs = MQTT_STUCK_DEADLINE_MS; /* 强制恢复前允许的断连时长；成功后归位，连续失败翻倍至 MQTT_SUPERVISE_MAX_MS */
    for (;;)
    {
        EventBits_t bits = xEventGroupWaitBits(g_wifiEventGroup,
                                               WIFI_DISCONNECTED_BIT | MQTT_AUTH_REFUSED_BIT,
                                               pdTRUE, pdFALSE,
                                               pdMS_TO_TICKS(MQTT_SUPERVISE_PERIOD_MS));

        /* 拒绝位优先：双位同至时一次全量恢复即可。若先走快路径，失效 token
         * 大概率又被拒，白烧 2 次 TLS 握手。 */
        bool full_reauth = (bits & MQTT_AUTH_REFUSED_BIT) != 0;
        bool wifi_was_down = (bits & WIFI_DISCONNECTED_BIT) != 0;
        bool supervise_force = false;

        if (wifi_was_down)
        {
            ESP_LOGW("main", "WiFi disconnected, waiting for reconnect...");
            g_isMQTTConnected = false;
            mqttDownSince = 0;      /* WiFi 掉线期间由下方恢复流程接管，不累计 MQTT 断连计时 */
            mqttStuckWaitMs = MQTT_STUCK_DEADLINE_MS; /* WiFi 恢复后重建全新连接，强制间隔归位 */

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
        else if (!full_reauth)
        {
            /* 纯 MQTT 断连监督：仅当 WiFi 在线且无认证拒绝事件时累计断连时长 */
            if ((xEventGroupGetBits(g_wifiEventGroup) & WIFI_CONNECTED_BIT) &&
                !g_isMQTTConnected)
            {
                if (mqttDownSince == 0)
                    mqttDownSince = xTaskGetTickCount();
                else if (xTaskGetTickCount() - mqttDownSince >= pdMS_TO_TICKS(mqttStuckWaitMs))
                {
                    ESP_LOGW("main",
                             "MQTT down for %u s while WiFi is up, forcing "
                             "supervised recovery (token reuse)",
                             (unsigned)(mqttStuckWaitMs / 1000));
                    mqttDownSince = 0;
                    supervise_force = true;
                    /* 仍连不上（broker 长时不可达，非 esp-mqtt 卡死）时退避翻倍，
                     * 避免无谓的反复毁建；一旦连上，else 分支会将间隔归位。 */
                    mqttStuckWaitMs *= 2U;
                    if (mqttStuckWaitMs > MQTT_SUPERVISE_MAX_MS)
                        mqttStuckWaitMs = MQTT_SUPERVISE_MAX_MS;
                }
            }
            else
            {
                mqttDownSince = 0;                    /* MQTT 已连接（或 WiFi 不在线）：清零计时 */
                mqttStuckWaitMs = MQTT_STUCK_DEADLINE_MS; /* 连接恢复，间隔归位 */
            }
        }

        /* 退避归零只存在于 run_recovery 成功返回之后：若在恢复动作前归零，
         * 恢复内部失败触发的重启永远只等 5s，风暴兜底形同虚设。
         * 恢复触发 = WiFi 掉线重连（快路径）/ CONNACK 拒绝（全量重认证）/ 监督超时（快路径）。 */
        if (wifi_was_down || full_reauth || supervise_force)
        {
            mqttDownSince = 0; /* 每次恢复都会重建全新连接，断连计时重新开始 */
            run_recovery(full_reauth);
        }
    }
}
