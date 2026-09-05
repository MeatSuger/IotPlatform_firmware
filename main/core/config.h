#ifndef _CONFIG_H_
#define _CONFIG_H_

/* --------------------------------------------------------------------------
 * Project configuration — all compile-time constants in one place.
 * -------------------------------------------------------------------------- */

/* Device identity */
#define DEVICE_ID         "90431b"

/* WiFi credentials */
#define WIFI_SSID         "yu"
#define WIFI_PASS         "56nauhcd"

/* Server API endpoints (HTTP) — 资源导向路径（GET/POST），见 back/api/swagger/API.md */
#define API_HOST          "api.meatsuger.top"
#define DATA_URL          "https://" API_HOST "/api/devices/" DEVICE_ID "/sensorData"
#define DATA_PULL_URL     "https://" API_HOST "/api/devices/" DEVICE_ID "/commands"
#define TOKEN_URL         "https://" API_HOST "/api/devices/" DEVICE_ID "/token"

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
