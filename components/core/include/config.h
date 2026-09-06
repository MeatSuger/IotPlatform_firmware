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
#define DATA_PULL_URL     "https://" API_HOST "/api/devices/" DEVICE_ID "/commands"
#define TOKEN_URL         "https://" API_HOST "/api/devices/" DEVICE_ID "/token"

/* MQTT broker (WSS) — split address fields instead of a full URI */
#define MQTT_BROKER_PATH      "/api/ws/mqtt/broker"

/* MQTT topics — 与后端 API.md §6.3 对齐：
 *  - iot/{id}/cmd       下行命令（QoS1，后端 EnqueueCmd 实时发布，HTTP /commands 兜底）
 *  - iot/{id}/config    云端配置快照（retained，订阅即拉取）
 *  - iot/{id}/config/report  配置回执上行
 *  - iot/{id}/telemetry 遥测上行（网关按此 topic 入库） */
#define MQTT_TOPIC_CMD          "iot/" DEVICE_ID "/cmd"
#define MQTT_TOPIC_CONFIG       "iot/" DEVICE_ID "/config"
#define MQTT_TOPIC_CONFIG_REPORT "iot/" DEVICE_ID "/config/report"
#define MQTT_TOPIC_DATA         "iot/" DEVICE_ID "/telemetry"

/* Default sensor report interval (ms) — 运行时值 g_reportIntervalMs，
 * 可由云端配置 sensor.reportInterval(秒) 覆盖 */
#define SEND_INTERVAL     30000U

/* Debug output — 1 = printf logging enabled, 0 = silenced */
#define DEBUG_ENABLE      1

#endif /* _CONFIG_H_ */
