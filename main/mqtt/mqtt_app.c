#include "mqtt_app.h"

#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "token.h"
#include "esp_crt_bundle.h" /* esp_crt_bundle_attach (IDF bundled CA bundle) */

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

esp_mqtt_client_handle_t mqtt_app_create(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker = {
            .address = {
                .hostname = API_HOST,
                .path = MQTT_BROKER_PATH,
                .port = 443,
                .transport = MQTT_TRANSPORT_OVER_WSS,
            },
            .verification = {
                .crt_bundle_attach = esp_crt_bundle_attach,
            },
        },
        .credentials = {
            .username = DEVICE_ID,
        },
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

void mqtt_app_start(esp_mqtt_client_handle_t client)
{
    if (client != NULL)
        esp_mqtt_client_start(client);
}

void mqtt_app_destroy(esp_mqtt_client_handle_t client)
{
    if (client != NULL)
    {
        esp_mqtt_client_stop(client);
        esp_mqtt_client_destroy(client);
    }
}

bool mqtt_publish(const char *topic, const char *payload, size_t len)
{
    /* Snapshot the shared handle to avoid use-after-free if the main
     * task destroys the MQTT client concurrently (TOCTOU). */
    esp_mqtt_client_handle_t client = g_mqttClient;

    if (!g_isMQTTConnected || client == NULL || topic == NULL ||
        payload == NULL)
        return false;

    int msg_id = esp_mqtt_client_publish(client, topic, payload, len, 0, 0);
    if (msg_id >= 0)
    {
        DEBUG_PRINT("MQTT published [msg_id=%d] to %s: %.*s\n",
                    msg_id, topic, (int)(len > 200 ? 200 : len), payload);
        return true;
    }
    return false;
}

bool mqtt_publish_response(const char *payload)
{
    if (!mqtt_publish(MQTT_TOPIC_RESP, payload, strlen(payload)))
    {
        DEBUG_PRINT("MQTT disconnected, response dropped: %s\n", payload);
        return false;
    }
    return true;
}
