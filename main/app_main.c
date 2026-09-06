#include "common.h"
#include "wifi.h"
#include "mqtt_app.h"
#include "app.h"
#include "network.h"
#include "sensor.h"
#include "token.h"
#include "periph.h"
#include "appcfg.h"

/* --------------------------------------------------------------------------
 * Global state definitions
 * -------------------------------------------------------------------------- */
esp_mqtt_client_handle_t g_mqttClient = NULL;
bool g_isMQTTConnected = false;
bool g_authSuccess = false;
volatile bool g_mqttAuthRefused = false;
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
    DEBUG_PRINT("[Core %d] Network init task started\n",
                (int)xPortGetCoreID());

    g_authSuccess = authbydeviceid();
    if (g_authSuccess)
        syncPendingCommands();

    xEventGroupSetBits(g_wifiEventGroup, NETWORK_INIT_DONE_BIT);
    DEBUG_PRINTLN("Network init task done (auth=%s)",
                  g_authSuccess ? "ok" : "fail");
    vTaskDelete(NULL);
}

/* --------------------------------------------------------------------------
 * reconnectAuthTask — re-auth in a dedicated task (same watchdog reason).
 * -------------------------------------------------------------------------- */
static void reconnectAuthTask(void *pv)
{
    DEBUG_PRINT("[Core %d] Re-auth task started\n",
                (int)xPortGetCoreID());

    token_clear();
    g_authSuccess = authbydeviceid();

    xEventGroupSetBits(g_wifiEventGroup, NETWORK_INIT_DONE_BIT);
    DEBUG_PRINTLN("Re-auth task done (ok=%s)",
                  g_authSuccess ? "yes" : "no");
    vTaskDelete(NULL);
}

/* --------------------------------------------------------------------------
 * recover_connection — 重新认证 + 离线命令兜底 + 重建 MQTT 客户端。
 *
 * 由两条路径共用：
 *   1. WiFi 断线重连后（Token 可能已按 30 天未上线被清理）；
 *   2. MQTT broker 拒绝连接（CONNACK refused = Token 失效/被顶号，见 g_mqttAuthRefused）。
 *
 * 步骤：重新获取 Token（DeviceIDAuth，专用任务避免 TLS 握手饿死 IDLE 触发看门狗）
 * → syncPendingCommands 兜底拉取离线期间下发的命令（拉取即消费）→ 用新 Token 重建客户端。
 * 认证失败：直接重启（重启后重新走完整接入流程）。
 * -------------------------------------------------------------------------- */
static void recover_connection(void)
{
    xTaskCreatePinnedToCore(reconnectAuthTask, "ReAuth", 8192,
                            NULL, 2, NULL, 0);
    EventBits_t rb = xEventGroupWaitBits(g_wifiEventGroup,
                                          NETWORK_INIT_DONE_BIT,
                                          pdFALSE, pdTRUE,
                                          pdMS_TO_TICKS(30000));
    if (!(rb & NETWORK_INIT_DONE_BIT) || !g_authSuccess)
    {
        DEBUG_PRINTLN("Re-auth failed, restarting...");
        esp_restart();
    }

    /* 离线命令兜底：拉取并处理平台命令队列（与 MQTT 实时通道互补）。
     * 需在重建 MQTT 前执行——retained config 快照重投与命令队列互不冲突。 */
    syncPendingCommands();

    /* Re-create MQTT client with the new token */
    if (g_mqttClient)
    {
        mqtt_app_destroy(g_mqttClient);
        g_mqttClient = NULL;  /* prevent use-after-free from other tasks */
    }
    g_mqttClient = mqtt_app_create();
    if (g_mqttClient == NULL)
    {
        DEBUG_PRINTLN("Failed to re-create MQTT client");
        esp_restart();
    }
    mqtt_app_start(g_mqttClient);
    DEBUG_PRINTLN("WiFi / MQTT recovered");
}

/* --------------------------------------------------------------------------
 * app_main — entry point
 * -------------------------------------------------------------------------- */
void app_main(void)
{
    DEBUG_PRINTLN("Device starting...");

    /* --- NVS (required by WiFi) --- */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* 设备配置持久化状态（NVS）：版本/载荷/待回执，MQTT 事件前就绪 */
    appcfg_init();

    /* Suppress PHY lib debug output — prevents printf lock crash in IDF 6.0 */
    esp_log_level_set("phy", ESP_LOG_NONE);
    esp_log_level_set("wifi", ESP_LOG_WARN);

    /* --- WiFi --- */
    if (!wifi_init_sta())
    {
        DEBUG_PRINTLN("WiFi connection failed, restarting...");
        esp_restart();
    }

    /* --- Command queue ---
     * 必须在 networkInitTask 之前创建：该任务里的 syncPendingCommands 会
     * 向队列入队离线命令（开机积压时必现，队列为 NULL 将触发断言崩溃）。
     * 深度 20：离线期间可积压多条控制/配置命令，cmdProcessTask 尚未启动时
     * 全部暂存于此（sync 按 id 升序逐条入队）。 */
    g_commandQueue = xQueueCreate(20, sizeof(CommandMsg));
    if (g_commandQueue == NULL)
    {
        DEBUG_PRINTLN("Failed to create command queue");
        esp_restart();
    }

    /* --- Authentication + pending-command sync ---
     * Run in a dedicated task: the TLS 1.3 ECDSA handshake on ESP32
     * (no hardware crypto) takes 5–8 s.  Doing it in app_main starves
     * IDLE0 and triggers the task watchdog at the default 5 s timeout. */
    xTaskCreatePinnedToCore(networkInitTask, "NetInit", 8192,
                            NULL, 2, NULL, 0);
    EventBits_t netBits = xEventGroupWaitBits(g_wifiEventGroup,
                                              NETWORK_INIT_DONE_BIT,
                                              pdFALSE, pdTRUE,
                                              pdMS_TO_TICKS(30000));
    if (!(netBits & NETWORK_INIT_DONE_BIT))
    {
        DEBUG_PRINTLN("Network init timeout, restarting...");
        esp_restart();
    }
    if (!g_authSuccess)
    {
        DEBUG_PRINTLN("Authentication failed, restarting...");
        esp_restart();
    }

    /* --- Temperature sensor --- */
    if (!sensor_init())
    {
        DEBUG_PRINTLN("Temperature sensor init failed, continuing anyway...");
    }

    /* --- Peripheral bus: register drivers --- */
    periph_bus_init();
    /* 从云端配置快照（NVS）重放执行器定义与上报周期 —— 配置定义唯一真源 */
    appcfg_replay();

    /* --- MQTT client (WSS) --- */
    g_mqttClient = mqtt_app_create();
    if (g_mqttClient == NULL)
    {
        DEBUG_PRINTLN("Failed to create MQTT client");
        esp_restart();
    }
    mqtt_app_start(g_mqttClient);

    /* --- Tasks --- */
    /* Core 0: periodic sensor report via MQTT */
    xTaskCreatePinnedToCore(sensorReportTask, "SensorReportTask", 10240,
                            NULL, 1, NULL, 0);

    /* Core 1: command processing */
    xTaskCreatePinnedToCore(cmdProcessTask, "CmdProcessTask", 8192,
                            NULL, 2, NULL, 1);

    DEBUG_PRINTLN("System init complete");

    /* The main thread becomes the Wi-Fi / MQTT watchdog (replaces loop()) */
    TickType_t lastReconnectLog = 0;
    for (;;)
    {
        EventBits_t bits = xEventGroupGetBits(g_wifiEventGroup);

        if (!(bits & WIFI_CONNECTED_BIT))
        {
            DEBUG_PRINTLN("WiFi disconnected, waiting for reconnect...");
            g_isMQTTConnected = false;

            /* Block until WiFi reconnects or 30 s timeout */
            bits = xEventGroupWaitBits(g_wifiEventGroup,
                                       WIFI_CONNECTED_BIT,
                                       pdFALSE, pdFALSE,
                                       pdMS_TO_TICKS(30000));
            if (!(bits & WIFI_CONNECTED_BIT))
            {
                DEBUG_PRINTLN("WiFi reconnection timeout, restarting...");
                esp_restart();
            }
            g_mqttAuthRefused = false; /* 全新连接，清认证拒绝标志 */
            recover_connection();
        }
        else if (g_mqttAuthRefused)
        {
            /* broker 拒绝连接 = Token 失效：立即停止 esp-mqtt 的无谓自动重连，
             * 重新取 Token 并重建客户端（WiFi 未断，无需等待）。 */
            DEBUG_PRINTLN("MQTT auth refused, re-authenticating...");
            g_mqttAuthRefused = false;
            recover_connection();
        }

        /* Log MQTT disconnect at most once per 10 s.
         * Use tick-based arithmetic to avoid 49.7-day wraparound bugs. */
        if (!g_isMQTTConnected && (bits & WIFI_CONNECTED_BIT))
        {
            TickType_t now = xTaskGetTickCount();
            if (now - lastReconnectLog > pdMS_TO_TICKS(10000))
            {
                DEBUG_PRINTLN("MQTT disconnected, waiting for auto-reconnect...");
                lastReconnectLog = now;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
