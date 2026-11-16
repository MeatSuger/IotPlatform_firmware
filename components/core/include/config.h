#ifndef _CONFIG_H_
#define _CONFIG_H_

/* 编译期常量 */

#define DEVICE_ID         "90431b"

/* 注册设备时后端只返回一次的引导密钥，首次取 token 时放在 X-Device-Secret。
 * 真实值放在本地 secrets.h(已 gitignore)，模板见 secrets.h.example。 */
#include "secrets.h"

/* 服务端 HTTP 接口，见 back/api/swagger/API.md */
#define API_HOST          "api.meatsuger.top"
#define DATA_PULL_URL     "https://" API_HOST "/api/devices/" DEVICE_ID "/commands"
#define TOKEN_URL         "https://" API_HOST "/api/devices/" DEVICE_ID "/token"
/* 上报面板芯片与分辨率 */
#define DEVICE_UPDATE_URL "https://" API_HOST "/api/devices/" DEVICE_ID "/update"

/* MQTT broker (WSS) */
#define MQTT_BROKER_PATH      "/api/ws/mqtt/broker"

/* MQTT 主题，与 API.md §6.3 对齐 */
#define MQTT_TOPIC_CMD          "iot/" DEVICE_ID "/cmd"
#define MQTT_TOPIC_CONFIG       "iot/" DEVICE_ID "/config"
#define MQTT_TOPIC_CONFIG_REPORT "iot/" DEVICE_ID "/config/report"
#define MQTT_TOPIC_DATA         "iot/" DEVICE_ID "/telemetry"

/* 图片分两条主题：位图分片可能以 '{' 开头，不能按首字节区分控制帧 */
#define MQTT_TOPIC_IMG_CTL      "iot/" DEVICE_ID "/img/ctl"
#define MQTT_TOPIC_IMG_DATA     "iot/" DEVICE_ID "/img/data"
#define MQTT_TOPIC_IMG_REPLY    "iot/" DEVICE_ID "/img/reply"
#define MQTT_IMG_CHUNK          4096

/* 收包缓冲：容纳一个图片分片或配置快照 JSON */
#define MQTT_BUFFER_SIZE        8192

/* 默认上报周期(ms)，云端 sensor.reportInterval 可覆盖 */
#define SEND_INTERVAL     30000U

/* 1 = modem-sleep + light-sleep；0 = 仅 DFS，射频常开 */
#define PWR_SAVE_ENABLE   1

/* 0.25 dBm 步进，52 = 13 dBm；合法范围 8..84 */
#define WIFI_MAX_TX_POWER_Q4      52

/* 保活 300s，减少 PING 唤醒 */
#define MQTT_KEEPALIVE_S          300

/* broker 不可达时的重连间隔，避免频繁 TLS 握手 */
#define MQTT_RECONNECT_TIMEOUT_MS 60000

/* esp-mqtt 内部重连无事件上报，靠主循环定时监督；
 * 断连超过 DEADLINE 强制重建，连续失败翻倍至 MAX */
#define MQTT_SUPERVISE_PERIOD_MS   30000U
#define MQTT_STUCK_DEADLINE_MS     180000U
#define MQTT_SUPERVISE_MAX_MS      900000U

/* 上电时拉低此脚擦除整个 NVS 并重启，回到配网模式。
 * 选 GPIO4：避开 strapping、USB-JTAG、片外 Flash、UART0 和墨水屏引脚 */
#define FACTORY_RESET_GPIO         4

#endif /* _CONFIG_H_ */
