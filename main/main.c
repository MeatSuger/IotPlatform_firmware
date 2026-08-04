#include "main.h"
#include "token.h"
#include "network.h"
#include "sensor.h"
#include "task.h"
#include "esp_wpa.h" /* esp_supplicant_disable_pmk_caching */
#include "esp_task_wdt.h"


/* --------------------------------------------------------------------------
 * Global state definitions
 * -------------------------------------------------------------------------- */
esp_mqtt_client_handle_t g_mqttClient = NULL;
bool g_isMQTTConnected = false;
bool g_authSuccess = false;
QueueHandle_t g_commandQueue = NULL;
EventGroupHandle_t g_wifiEventGroup = NULL;

/* --------------------------------------------------------------------------
 * WiFi event handler — signals the event group when connected / disconnected
 * -------------------------------------------------------------------------- */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        DEBUG_PRINTLN("WiFi disconnected");
        if (g_wifiEventGroup != NULL)
            xEventGroupClearBits(g_wifiEventGroup, WIFI_CONNECTED_BIT);
        esp_wifi_connect();
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        DEBUG_PRINTLN("WiFi connected, IP: " IPSTR,
                      IP2STR(&event->ip_info.ip));
        if (g_wifiEventGroup != NULL)
            xEventGroupSetBits(g_wifiEventGroup, WIFI_CONNECTED_BIT);
    }
}

static bool init_wifi(void)
{

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL,
        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL,
        &instance_got_ip));

    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    assert(sta_netif);

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };
    /* Create the event group BEFORE starting WiFi — the async event
     * handler (which fires on esp_wifi_start) needs it to exist. */
    g_wifiEventGroup = xEventGroupCreate();
    if (g_wifiEventGroup == NULL)
    {
        DEBUG_PRINTLN("Failed to create WiFi event group");
        return false;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    /* Disable PMK caching BEFORE starting WiFi to avoid known IDF 6.0
     * PMKSA NULL-deref crash (espressif/esp-idf#15584, commit 437fa9a) */
    esp_supplicant_disable_pmk_caching(true);

    ESP_ERROR_CHECK(esp_wifi_start());

    /* Disable power save to avoid EAPOL rekey crash (IDF 6.0 bug) */
    esp_wifi_set_ps(WIFI_PS_NONE);

    DEBUG_PRINTLN("Connecting to Wi-Fi: %s", WIFI_SSID);

    /* Block until connected or 15 s timeout */
    EventBits_t bits = xEventGroupWaitBits(g_wifiEventGroup,
                                           WIFI_CONNECTED_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(15000));
    if (bits & WIFI_CONNECTED_BIT)
    {
        DEBUG_PRINTLN("WiFi connected successfully");
        return true;
    }
    else
    {
        DEBUG_PRINTLN("WiFi connection timeout");
        return false;
    }
}

/* --------------------------------------------------------------------------
 * mqtt_event_handler — callback for esp_mqtt_client events
 * -------------------------------------------------------------------------- */
static void mqtt_event_handler(void *handler_args,
                               esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;

    switch (event->event_id)
    {
    case MQTT_EVENT_CONNECTED:
        DEBUG_PRINTLN("MQTT connected");
        g_isMQTTConnected = true;

        /* Subscribe to command topic */
        esp_mqtt_client_subscribe(event->client, MQTT_TOPIC_CMD, 0);
        DEBUG_PRINTLN("Subscribed to: %s", MQTT_TOPIC_CMD);
        break;

    case MQTT_EVENT_DISCONNECTED:
        DEBUG_PRINTLN("MQTT disconnected");
        g_isMQTTConnected = false;
        /* Drain queue and free pending payloads */
        {
            CommandMsg msg;
            while (xQueueReceive(g_commandQueue, &msg, 0) == pdTRUE)
                free(msg.payload);
        }
        break;

    case MQTT_EVENT_DATA:
    {
        DEBUG_PRINTLN("MQTT message received, topic: %.*s, data: %.*s",
                      event->topic_len, event->topic,
                      event->data_len, event->data);

        /* Only process messages on the command topic.
         * event->topic is NOT guaranteed null-terminated — use topic_len. */
        size_t cmd_topic_len = strlen(MQTT_TOPIC_CMD);
        if (event->topic_len != cmd_topic_len ||
            strncmp(event->topic, MQTT_TOPIC_CMD, cmd_topic_len) != 0)
            break;

        CommandMsg cmdMsg;
        cmdMsg.payload = malloc(event->data_len + 1);
        if (cmdMsg.payload == NULL)
        {
            DEBUG_PRINTLN("OOM: cannot allocate command payload");
            break;
        }
        memcpy(cmdMsg.payload, event->data, event->data_len);
        cmdMsg.payload[event->data_len] = '\0';
        cmdMsg.length = event->data_len;

        if (xQueueSend(g_commandQueue, &cmdMsg, 0) != pdPASS)
        {
            DEBUG_PRINTLN("Command queue full, dropping message");
            free(cmdMsg.payload);
        }
        break;
    }

    case MQTT_EVENT_ERROR:
        DEBUG_PRINTLN("MQTT error");
        g_isMQTTConnected = false;
        break;

    default:
        break;
    }
}

/* --------------------------------------------------------------------------
 * create_mqtt_client — allocate and configure the MQTT client (WSS transport).
 * -------------------------------------------------------------------------- */
static esp_mqtt_client_handle_t create_mqtt_client(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_BROKER_URI,
        .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,
        .credentials.username = DEVICE_ID,
    };

    /* If we have an auth token, use it as the MQTT password.
     * NOTE: the MQTT client stores the password pointer directly —
     * the underlying data must outlive the client.  Do NOT free(token)
     * here; it will be freed when the MQTT client is destroyed. */
    char *token = token_load();
    if (token != NULL && strlen(token) > 0)
    {
        mqtt_cfg.credentials.authentication.password = token;
    }

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    if (client == NULL)
    {
        free(token);
        return NULL;
    }

    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID,
                                   mqtt_event_handler, NULL);
    return client;
}

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
    if (!init_wifi())
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

    /* --- Command queue --- */
    g_commandQueue = xQueueCreate(10, sizeof(CommandMsg));
    if (g_commandQueue == NULL)
    {
        DEBUG_PRINTLN("Failed to create command queue");
        esp_restart();
    }

    /* --- MQTT client (WSS) --- */
    g_mqttClient = create_mqtt_client();
    if (g_mqttClient == NULL)
    {
        DEBUG_PRINTLN("Failed to create MQTT client");
        esp_restart();
    }
    esp_mqtt_client_start(g_mqttClient);
    
    
    /* --- Tasks --- */
    /* Core 0: periodic HTTP upload */
    xTaskCreatePinnedToCore(httpUploadTask, "HttpUploadTask", 10240,
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
                esp_mqtt_client_stop(g_mqttClient);
                esp_mqtt_client_destroy(g_mqttClient);
                g_mqttClient = NULL;  /* prevent use-after-free from other tasks */
            }
            g_mqttClient = create_mqtt_client();
            if (g_mqttClient == NULL)
            {
                DEBUG_PRINTLN("Failed to re-create MQTT client");
                esp_restart();
            }
            esp_mqtt_client_start(g_mqttClient);
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
