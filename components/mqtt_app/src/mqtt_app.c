#include "mqtt_app.h"

#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "esp_log.h"
#include "token.h"
#include "esp_crt_bundle.h" /* esp_crt_bundle_attach (IDF bundled CA bundle) */

/* esp-mqtt 组件内部 ESP_EVENT_DEFINE_BASE(MQTT_EVENTS)，但未在 public header 导出，
 * 这里按 esp_event 惯例自行声明，用于在回调里校验 event base。 */
ESP_EVENT_DECLARE_BASE(MQTT_EVENTS);

static const char *TAG = "mqtt";

/* --------------------------------------------------------------------------
 * mqtt_event_handler — callback for esp_mqtt_client events
 * -------------------------------------------------------------------------- */
static bool topic_matches(size_t topic_len, const char *topic,
                          const char *wanted)
{
    size_t wanted_len = strlen(wanted);
    return topic_len == wanted_len &&
           strncmp(topic, wanted, wanted_len) == 0;
}

static void mqtt_event_handler(void *handler_args,
                               esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    esp_mqtt_client_handle_t client = handler_args;
    if (base != MQTT_EVENTS) /* 防御：本模块仅注册于 MQTT_EVENTS base */
        return;

    esp_mqtt_event_handle_t event = event_data;

    switch (event_id)
    {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT connected");
        g_isMQTTConnected = true;
        if (g_wifiEventGroup != NULL)
            xEventGroupClearBits(g_wifiEventGroup, MQTT_AUTH_REFUSED_BIT); /* broker 接受了凭证 */

        /* Subscribe QoS1：实时命令/离线后 HTTP 兜底 */
        esp_mqtt_client_subscribe(client, MQTT_TOPIC_CMD, 1);
        ESP_LOGI(TAG, "Subscribed to: %s", MQTT_TOPIC_CMD);

        /* Subscribe QoS1：订阅即收到 retained 最新配置快照 */
        esp_mqtt_client_subscribe(client, MQTT_TOPIC_CONFIG, 1);
        ESP_LOGI(TAG, "Subscribed to: %s", MQTT_TOPIC_CONFIG);
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT disconnected");
        g_isMQTTConnected = false;
        /* 不 drain 命令队列：队列消费者 cmdProcessTask 与传输层无关，断线清空
         * 无正确性依据，反而会冲掉 HTTP 兜底刚同步入队的离线命令；队列深 20，
         * 满时已有入队侧显式丢弃日志。 */
        break;

    case MQTT_EVENT_DATA:
    {
        ESP_LOGD(TAG, "MQTT message received, topic: %.*s, data: %.*s",
                      event->topic_len, event->topic,
                      event->data_len, event->data);

        /* Only process messages on the command / config topics.
         * event->topic is NOT guaranteed null-terminated — use topic_len. */
        if (!topic_matches(event->topic_len, event->topic, MQTT_TOPIC_CMD) &&
            !topic_matches(event->topic_len, event->topic, MQTT_TOPIC_CONFIG))
            break;

        CommandMsg cmdMsg;
        cmdMsg.payload = malloc(event->data_len + 1);
        if (cmdMsg.payload == NULL)
        {
            ESP_LOGE(TAG, "OOM: cannot allocate command payload");
            break;
        }
        memcpy(cmdMsg.payload, event->data, event->data_len);
        cmdMsg.payload[event->data_len] = '\0';
        cmdMsg.length = event->data_len;

        if (xQueueSend(g_commandQueue, &cmdMsg, 0) != pdPASS)
        {
            ESP_LOGW(TAG, "Command queue full, dropping message");
            free(cmdMsg.payload);
        }
        break;
    }

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT error");
        g_isMQTTConnected = false;
        /* 连接被 broker 拒绝（CONNACK 非 accepted）：用户名=DEVICE_ID 固定，
         * 拒绝即密码（设备 Token）失效/被清理。置位事件后由 app_main 主循环触发
         * 重新取 Token + 重建客户端，避免 esp-mqtt 无限自动重连失败。 */
        if (event->error_handle != NULL &&
            event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED)
        {
            ESP_LOGW(TAG, "MQTT connection refused (return_code=%d), token likely stale",
                          (int)event->error_handle->connect_return_code);
            if (g_wifiEventGroup != NULL)
                xEventGroupSetBits(g_wifiEventGroup, MQTT_AUTH_REFUSED_BIT);
        }
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
        .session = {
            .keepalive = MQTT_KEEPALIVE_S,
        },
        .network = {
            .reconnect_timeout_ms = MQTT_RECONNECT_TIMEOUT_MS,
        },
        .credentials = {
            .username = DEVICE_ID,
        },
    };

    /* 用 auth token（如有）作为 MQTT 密码。esp-mqtt 在 init 内部 strdup 了
     * password，destroy 时释放的是那份副本，因此 init 返回后本地的 token
     * 副本即可释放，无需存活到客户端销毁。 */
    char *token = token_load();
    if (token != NULL && strlen(token) > 0)
    {
        mqtt_cfg.credentials.authentication.password = token;
    }

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    free(token);
    if (client == NULL)
        return NULL;

    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID,
                                   mqtt_event_handler, client);
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
        ESP_LOGI(TAG, "MQTT published [msg_id=%d] to %s: %.*s",
                    msg_id, topic, (int)(len > 200 ? 200 : len), payload);
        return true;
    }
    return false;
}
