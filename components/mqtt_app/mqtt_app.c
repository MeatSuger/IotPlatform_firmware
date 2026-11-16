#include "mqtt_app.h"

#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "esp_crt_bundle.h" /* IDF bundled CA bundle */
#include "esp_log.h"
#include "img_recv.h" /* 图片接收（MQTT 直传 → FatFS → 墨水屏） */
#include "token.h"

/* esp-mqtt 未导出 MQTT_EVENTS base */
ESP_EVENT_DECLARE_BASE(MQTT_EVENTS);

static const char *TAG = "mqtt";

static bool topic_matches(size_t topic_len, const char *topic,
                          const char *wanted) {
  size_t wanted_len = strlen(wanted);
  return topic_len == wanted_len && strncmp(topic, wanted, wanted_len) == 0;
}

/* 图片分片按 offset/total 重组后交给 img 组件 */
static char s_img_rx[MQTT_BUFFER_SIZE];
static int s_img_rx_len;

/* cmd/config 可能被分片，整条收齐再入队 */
#define MQTT_CMD_RX_MAX (MQTT_BUFFER_SIZE * 2)
static char s_cmd_rx[MQTT_CMD_RX_MAX];
static int s_cmd_rx_len;

static void img_mqtt_handle(esp_mqtt_event_handle_t event) {
  const bool is_ctl =
      topic_matches(event->topic_len, event->topic, MQTT_TOPIC_IMG_CTL);
  const bool is_data =
      topic_matches(event->topic_len, event->topic, MQTT_TOPIC_IMG_DATA);

  if (event->current_data_offset == 0)
    s_img_rx_len = 0;

  if (s_img_rx_len + event->data_len > (int)sizeof(s_img_rx)) {
    ESP_LOGE(TAG, "img rx overflow (%d + %d), drop", s_img_rx_len,
             event->data_len);
    s_img_rx_len = 0;
    return;
  }
  memcpy(s_img_rx + s_img_rx_len, event->data, event->data_len);
  s_img_rx_len += event->data_len;

  if (event->current_data_offset + event->data_len < event->total_data_len)
    return;

  if (is_ctl)
    img_recv_handle(IMG_FRAME_TEXT, (const uint8_t *)s_img_rx, s_img_rx_len);
  else if (is_data)
    img_recv_handle(IMG_FRAME_BINARY, (const uint8_t *)s_img_rx, s_img_rx_len);

  s_img_rx_len = 0;
}

/* esp-mqtt 内部重连无事件，卡死只能由主循环监督 */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data) {
  esp_mqtt_client_handle_t client = handler_args;
  if (base != MQTT_EVENTS) /* 只注册了 MQTT_EVENTS */
    return;

  esp_mqtt_event_handle_t event = event_data;

  switch (event_id) {
  case MQTT_EVENT_CONNECTED:
    ESP_LOGI(TAG, "MQTT connected");
    g_isMQTTConnected = true;
    if (g_wifiEventGroup != NULL)
      xEventGroupClearBits(g_wifiEventGroup,
                           MQTT_AUTH_REFUSED_BIT); /* broker 接受了凭证 */

    /* cmd: QoS1，非 retained */
    esp_mqtt_client_subscribe(client, MQTT_TOPIC_CMD, 1);
    ESP_LOGI(TAG, "Subscribed to: %s", MQTT_TOPIC_CMD);

    /* config: retained */
    esp_mqtt_client_subscribe(client, MQTT_TOPIC_CONFIG, 1);
    ESP_LOGI(TAG, "Subscribed to: %s", MQTT_TOPIC_CONFIG);

    /* img: QoS0，不重传 */
    esp_mqtt_client_subscribe(client, MQTT_TOPIC_IMG_CTL, 0);
    esp_mqtt_client_subscribe(client, MQTT_TOPIC_IMG_DATA, 0);
    ESP_LOGI(TAG, "Subscribed to: %s, %s", MQTT_TOPIC_IMG_CTL,
             MQTT_TOPIC_IMG_DATA);
    break;

  case MQTT_EVENT_DISCONNECTED:
    ESP_LOGW(TAG, "MQTT disconnected");
    g_isMQTTConnected = false;
    /* 不清队列，保留 HTTP 拉取的命令 */
    break;

  case MQTT_EVENT_DATA: {
    ESP_LOGD(TAG, "MQTT message received, topic: %.*s, data: %.*s",
             event->topic_len, event->topic, event->data_len, event->data);

    /* 图片走 img 组件，不进命令队列 */
    if (topic_matches(event->topic_len, event->topic, MQTT_TOPIC_IMG_CTL) ||
        topic_matches(event->topic_len, event->topic, MQTT_TOPIC_IMG_DATA)) {
      img_mqtt_handle(event);
      break;
    }

    /* topic 不保证 NUL 结尾，比较用 topic_len */
    if (!topic_matches(event->topic_len, event->topic, MQTT_TOPIC_CMD) &&
        !topic_matches(event->topic_len, event->topic, MQTT_TOPIC_CONFIG))
      break;

    if (event->current_data_offset == 0)
      s_cmd_rx_len = 0;

    if (s_cmd_rx_len + event->data_len > (int)sizeof(s_cmd_rx)) {
      ESP_LOGE(TAG, "cmd/config rx overflow (%d + %d), drop", s_cmd_rx_len,
               event->data_len);
      s_cmd_rx_len = 0;
      break;
    }
    memcpy(s_cmd_rx + s_cmd_rx_len, event->data, event->data_len);
    s_cmd_rx_len += event->data_len;

    if (event->current_data_offset + event->data_len < event->total_data_len)
      break;

    if (g_commandQueue == NULL) {
      ESP_LOGW(TAG, "Command queue not ready, drop message");
      s_cmd_rx_len = 0;
      break;
    }

    CommandMsg cmdMsg;
    cmdMsg.payload = malloc(s_cmd_rx_len + 1);
    if (cmdMsg.payload == NULL) {
      ESP_LOGE(TAG, "OOM: cannot allocate command payload");
      s_cmd_rx_len = 0;
      break;
    }
    memcpy(cmdMsg.payload, s_cmd_rx, s_cmd_rx_len);
    cmdMsg.payload[s_cmd_rx_len] = '\0';
    cmdMsg.length = s_cmd_rx_len;
    s_cmd_rx_len = 0;

    if (xQueueSend(g_commandQueue, &cmdMsg, 0) != pdPASS) {
      ESP_LOGW(TAG, "Command queue full, dropping message");
      free(cmdMsg.payload);
    }
    break;
  }

  case MQTT_EVENT_ERROR:
    ESP_LOGE(TAG, "MQTT error");
    g_isMQTTConnected = false;
    /* CONNACK 拒绝 = Token 失效，通知 app_main 全量重认证 */
    if (event->error_handle != NULL &&
        event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
      ESP_LOGW(TAG,
               "MQTT connection refused (return_code=%d), token likely stale",
               (int)event->error_handle->connect_return_code);
      if (g_wifiEventGroup != NULL)
        xEventGroupSetBits(g_wifiEventGroup, MQTT_AUTH_REFUSED_BIT);
    }
    break;

  default:
    break;
  }
}

/* img 回执 → img/reply */
static int img_reply(const char *text) {
  return mqtt_publish(MQTT_TOPIC_IMG_REPLY, text, strlen(text)) ? 0 : -1;
}

esp_mqtt_client_handle_t mqtt_app_create(void) {
  /* keepalive 300s、重连 60s，减少 TLS 全握手 */
  esp_mqtt_client_config_t mqtt_cfg = {
      .broker =
          {
              .address =
                  {
                      .hostname = API_HOST,
                      .path = MQTT_BROKER_PATH,
                      .port = 443,
                      .transport = MQTT_TRANSPORT_OVER_WSS,
                  },
              .verification =
                  {
                      .crt_bundle_attach = esp_crt_bundle_attach,
                  },
          },
      .session =
          {
              .keepalive = MQTT_KEEPALIVE_S,
          },
      .network =
          {
              .reconnect_timeout_ms = MQTT_RECONNECT_TIMEOUT_MS,
          },
      .buffer =
          {
              /* 默认 1024 装不下图片分片或配置快照 */
              .size = MQTT_BUFFER_SIZE,
          },
      .credentials =
          {
              .username = DEVICE_ID,
          },
  };

  /* esp-mqtt 内部 strdup，init 后即可释放 */
  char *token = token_load();
  if (token != NULL && strlen(token) > 0) {
    mqtt_cfg.credentials.authentication.password = token;
  }

  esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
  free(token);
  if (client == NULL)
    return NULL;

  /* 挂 storage、建刷屏任务；失败只降级图片功能 */
  esp_err_t img_err = img_recv_start(img_reply);
  if (img_err != ESP_OK)
    ESP_LOGW(TAG, "img_recv_start failed: %s (图片传输不可用)",
             esp_err_to_name(img_err));

  esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler,
                                 client);
  return client;
}

void mqtt_app_start(esp_mqtt_client_handle_t client) {
  if (client != NULL)
    esp_mqtt_client_start(client);
}

void mqtt_app_destroy(esp_mqtt_client_handle_t client) {
  if (client != NULL) {
    esp_mqtt_client_stop(client);
    esp_mqtt_client_destroy(client);
  }
}

bool mqtt_publish(const char *topic, const char *payload, size_t len) {
  /* 快照句柄，防主任务并发 destroy 造成 use-after-free */
  esp_mqtt_client_handle_t client = g_mqttClient;

  if (!g_isMQTTConnected || client == NULL || topic == NULL || payload == NULL)
    return false;

  int msg_id = esp_mqtt_client_publish(client, topic, payload, len, 0, 0);
  if (msg_id >= 0) {
    ESP_LOGI(TAG, "MQTT published [msg_id=%d] to %s: %.*s", msg_id, topic,
             (int)(len > 200 ? 200 : len), payload);
    return true;
  }
  return false;
}
