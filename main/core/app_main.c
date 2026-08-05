#include "common.h"
#include "wifi.h"
#include "mqtt_app.h"
#include "app.h"
#include "network.h"
#include "sensor.h"
#include "token.h"
#include "periph.h"

/* --------------------------------------------------------------------------
 * Global state definitions
 * -------------------------------------------------------------------------- */
esp_mqtt_client_handle_t g_mqttClient = NULL;
bool g_isMQTTConnected = false;
bool g_authSuccess = false;
QueueHandle_t g_commandQueue = NULL;
EventGroupHandle_t g_wifiEventGroup = NULL;

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

    /* Suppress PHY lib debug output — prevents printf lock crash in IDF 6.0 */
    esp_log_level_set("phy", ESP_LOG_NONE);
    esp_log_level_set("wifi", ESP_LOG_WARN);

    /* --- WiFi --- */
    if (!wifi_init_sta())
    {
        DEBUG_PRINTLN("WiFi connection failed, restarting...");
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

    /* --- Peripheral bus: register drivers, restore persisted devices --- */
    periph_bus_init();
    periph_devices_load();

    /* --- Command queue --- */
    g_commandQueue = xQueueCreate(10, sizeof(CommandMsg));
    if (g_commandQueue == NULL)
    {
        DEBUG_PRINTLN("Failed to create command queue");
        esp_restart();
    }

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

            /* Re-auth in a dedicated task (same TWDT-avoidance as init).
             * The main task blocks on the event group, allowing IDLE0 to
             * run while the helper task does the TLS handshake. */
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
    