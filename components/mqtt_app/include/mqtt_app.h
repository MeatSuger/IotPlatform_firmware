#ifndef _MQTT_APP_H_
#define _MQTT_APP_H_

#include <stdbool.h>
#include <stddef.h>
#include "mqtt_client.h"

/* --------------------------------------------------------------------------
 * MQTT application layer: client lifecycle + publish helpers.
 *
 * - create: allocate and configure the client (WSS transport, auth
 *   token as password) and register the event handler. The handler
 *   subscribes to MQTT_TOPIC_CMD / MQTT_TOPIC_CONFIG on connect and
 *   pushes incoming messages into g_commandQueue (consumer sniffs the
 *   envelope: command vs config snapshot).
 * - start/stop/destroy: explicit lifecycle used by app_main (also for
 *   re-creating the client after WiFi reconnects).
 *
 * All publish helpers snapshot the shared client handle internally
 * (TOCTOU protection: app_main may destroy/recreate the client on WiFi
 * reconnect) and check the connection flag first.
 * -------------------------------------------------------------------------- */

/* MQTT 客户端句柄与连接状态（定义于 main/app_main.c，由 mqtt 组件独占维护）。
 * g_isMQTTConnected 写者 = MQTT 事件回调任务，读者 = 主任务/上报任务：volatile */
extern esp_mqtt_client_handle_t g_mqttClient;
extern volatile bool g_isMQTTConnected;

/* Create and configure the MQTT client (does not start it).
 * Returns NULL on failure. */
esp_mqtt_client_handle_t mqtt_app_create(void);

/* Start the client (async connect). */
void mqtt_app_start(esp_mqtt_client_handle_t client);

/* Stop and destroy the client. */
void mqtt_app_destroy(esp_mqtt_client_handle_t client);

/* Publish a payload to an arbitrary topic. */
bool mqtt_publish(const char *topic, const char *payload, size_t len);

#endif /* _MQTT_APP_H_ */
