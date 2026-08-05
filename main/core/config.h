#ifndef _CONFIG_H_
#define _CONFIG_H_

/* --------------------------------------------------------------------------
 * Project configuration — all compile-time constants in one place.
 * -------------------------------------------------------------------------- */

/* Device identity */
#define DEVICE_ID         "90431b"

/* WiFi credentials */
#define WIFI_SSID         "C9-1101"
#define WIFI_PASS         "printf%d1"

/* Server API endpoints (HTTP) */
#define API_HOST          "api.meatsuger.top"
#define DATA_URL          "https://" API_HOST "/api/data/" DEVICE_ID "/Data"
#define DATA_PULL_URL     "https://" API_HOST "/api/device/" DEVICE_ID "/cmd"
#define TOKEN_URL         "https://" API_HOST "/api/device/" DEVICE_ID "/login"

/* MQTT broker (WSS) — split address fields instead of a full URI */
#define MQTT_BROKER_PATH      "/api/ws/mqtt/broker"

/* MQTT topics */
#define MQTT_TOPIC_CMD    "device/" DEVICE_ID "/cmd"
#define MQTT_TOPIC_RESP   "device/" DEVICE_ID "/resp"
#define MQTT_TOPIC_DATA   "device/" DEVICE_ID "/data"

/* Sensor report interval (ms) */
#define SEND_INTERVAL     30000U

/* Debug output — 1 = printf logging enabled, 0 = silenced */
#define DEBUG_ENABLE      1

#endif /* _CONFIG_H_ */
