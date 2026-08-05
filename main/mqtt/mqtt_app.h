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
 *   subscribes to MQTT_TOPIC_CMD on connect and pushes incoming
 *   commands into g_commandQueue.
 * - start/stop/destroy: explicit lifecycle used by app_main (also for
 *   re-creating the client after WiFi reconnects).
 *
 * All publish helpers snapshot the shared client handle internally
 * (TOCTOU protection: app_main may destroy/recreate the client on WiFi
 * reconnect) and check the connection flag first.
 * -------------------------------------------------------------------------- */

/* Create and configure the MQTT client (does not start it).
 * Returns NULL on failure. */
esp_mqtt_client_handle_t mqtt_app_create(void);

/* Start the client (async connect). */
void mqtt_app_start(esp_mqtt_client_handle_t client);

/* Stop and destroy the client. */
void mqtt_app_destroy(esp_mqtt_client_handle_t client);

/* Publish a payload to an arbitrary topic. */
bool mqtt_publish(const char *topic, const char *payload, size_t len);

/* Publish a payload to the response topic (MQTT_TOPIC_RESP). */
bool mqtt_publish_response(const char *payload);

#endif /* _MQTT_APP_H_ */
