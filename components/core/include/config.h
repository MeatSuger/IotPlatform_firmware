#ifndef _CONFIG_H_
#define _CONFIG_H_

/* --------------------------------------------------------------------------
 * Project configuration — all compile-time constants in one place.
 * -------------------------------------------------------------------------- */

#define DEVICE_ID         "90431b"

#define WIFI_SSID         "yu"
#define WIFI_PASS         "56nauhcd"

/* Server API endpoints (HTTP) — 资源导向路径（GET/POST），见 back/api/swagger/API.md */
#define API_HOST          "api.meatsuger.top"
#define DATA_PULL_URL     "https://" API_HOST "/api/devices/" DEVICE_ID "/commands"
#define TOKEN_URL         "https://" API_HOST "/api/devices/" DEVICE_ID "/token"

/* MQTT broker (WSS) — address 拆成字段而非整 URI */
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

/* 1 = WiFi modem-sleep + CPU light-sleep（命令下行延迟≈1 个 DTIM，平均 ~50ms）；
 * 0 = 仅 DFS 降频、射频常开（硬实时） */
#define PWR_SAVE_ENABLE   1

/* WiFi 最大发射功率，0.25 dBm 步进：52 = 13 dBm（边缘 IoT 典型档），
 * 穿墙/远距可上调至 78（19.5 dBm），合法范围 8..84 */
#define WIFI_MAX_TX_POWER_Q4      52

/* MQTT 保活（秒）：默认 120 → 300，减少保活 PING 的射频唤醒；需 broker 容忍 */
#define MQTT_KEEPALIVE_S          300

/* broker 不可达时 esp-mqtt 重连间隔（默认 ~10s）：避免频繁 TLS 全握手（5-8s 高功耗） */
#define MQTT_RECONNECT_TIMEOUT_MS 60000

/* 主循环 MQTT 监督（防 esp-mqtt 内部 auto-reconnect 静默失效后无人兜底）：
 * esp-mqtt 的重连尝试不向应用层抛事件，一旦其内部重连（TCP 半开 / 陈旧内核
 * socket / TLS、DNS 瞬时异常）卡死，WiFi 在线时本固件收不到任何信号、永不复连。
 * - MQTT_SUPERVISE_PERIOD_MS   监督唤醒周期：主循环周期性醒来检查
 *   "WiFi 在线但 MQTT 未连接"，唤醒耗电与 DTIM 周期唤醒相比可忽略。
 * - MQTT_STUCK_DEADLINE_MS     WiFi 在线且 MQTT 连续断连超过该时长 → 强制一次
 *   token-reuse 恢复（毁旧建新 = 全新 TCP/TLS/DNS + 重新订阅）。取值须大于
 *   esp-mqtt 自然重试周期（reconnect_timeout 60s + 连接超时裕量），避免干扰库内
 *   正常重连；CONNACK 拒绝仍走既有的全量重认证路径（见 app_main）。
 * - MQTT_SUPERVISE_MAX_MS      连续强制恢复仍失败（broker 长时不可达，非 esp-mqtt
 *   卡死）时，强制间隔翻倍的上限；连接成功即回到 MQTT_STUCK_DEADLINE_MS。 */
#define MQTT_SUPERVISE_PERIOD_MS   30000U
#define MQTT_STUCK_DEADLINE_MS     180000U
#define MQTT_SUPERVISE_MAX_MS      900000U

#endif /* _CONFIG_H_ */
