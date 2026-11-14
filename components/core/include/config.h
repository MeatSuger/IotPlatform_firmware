#ifndef _CONFIG_H_
#define _CONFIG_H_

/* 项目编译期常量集中于此（取值理由与硬件约束见各常量上方注释） */

#define DEVICE_ID         "90431b"

/* WiFi 配网（SmartConfig / ESP-Touch + AirKiss）——编译期不内置 SSID/密码：首次
 * 上电（NVS 无已配网标记）进入配网模式，手机连目标 2.4G WiFi 后用 Espressif
 * ESP Touch / AirKiss 广播凭据，设备捕获经 esp_wifi_set_config 存入 WiFi NVS 并以
 * "wifi"/"provisioned" 标记已配网，后续上电直连；重新配网擦除 NVS（如 erase-flash）即可。 */

/* Server API endpoints (HTTP) — 资源导向路径（GET/POST），见 back/api/swagger/API.md */
#define API_HOST          "api.meatsuger.top"
#define DATA_PULL_URL     "https://" API_HOST "/api/devices/" DEVICE_ID "/commands"
#define TOKEN_URL         "https://" API_HOST "/api/devices/" DEVICE_ID "/token"

/* MQTT broker (WSS) — address 拆成字段而非整 URI */
#define MQTT_BROKER_PATH      "/api/ws/mqtt/broker"

/* MQTT topics — 与后端 API.md §6.3 对齐（iot/{id}/...）：cmd 下行命令（QoS1，后端
 * EnqueueCmd 实时发布，HTTP /commands 兜底）；config 配置快照（retained，订阅即
 * 拉取）；config/report 配置回执上行；telemetry 遥测上行（网关按此 topic 入库）。 */
#define MQTT_TOPIC_CMD          "iot/" DEVICE_ID "/cmd"
#define MQTT_TOPIC_CONFIG       "iot/" DEVICE_ID "/config"
#define MQTT_TOPIC_CONFIG_REPORT "iot/" DEVICE_ID "/config/report"
#define MQTT_TOPIC_DATA         "iot/" DEVICE_ID "/telemetry"

/* 图片传输（墨水屏，见 components/img）——控制帧与二进制分片分两条主题：合并成一条
 * 靠「首字节是否 '{'」区分会踩坑（位图分片可能恰以 0x7B 开头，被当控制帧、JSON 解析
 * 失败整片丢弃）：img/ctl JSON 控制帧 img_start/img_end；img/data 二进制分片（每片
 * MQTT_IMG_CHUNK 字节，须小于收包缓冲）；img/reply 回执 img_ready/img_done/img_shown/img_error。 */
#define MQTT_TOPIC_IMG_CTL      "iot/" DEVICE_ID "/img/ctl"
#define MQTT_TOPIC_IMG_DATA     "iot/" DEVICE_ID "/img/data"
#define MQTT_TOPIC_IMG_REPLY    "iot/" DEVICE_ID "/img/reply"
#define MQTT_IMG_CHUNK          4096

/* MQTT 收包缓冲（字节）：须容纳单条完整分片（MQTT_IMG_CHUNK）以及配置快照 JSON */
#define MQTT_BUFFER_SIZE        8192

/* 传感器上报周期（ms），运行时值 g_reportIntervalMs，可由云端 sensor.reportInterval(秒) 覆盖 */
#define SEND_INTERVAL     30000U

/* 1 = WiFi modem-sleep + CPU light-sleep（命令下行延迟≈1 个 DTIM，平均 ~50ms）；
 * 0 = 仅 DFS 降频、射频常开（硬实时） */
#define PWR_SAVE_ENABLE   1

/* WiFi 最大发射功率，0.25 dBm 步进：52 = 13 dBm；穿墙/远距可上调至 78（19.5 dBm），合法范围 8..84 */
#define WIFI_MAX_TX_POWER_Q4      52

/* MQTT 保活（秒）：默认 120 → 300，减少保活 PING 的射频唤醒；需 broker 容忍 */
#define MQTT_KEEPALIVE_S          300

/* broker 不可达时 esp-mqtt 重连间隔（默认 ~10s）：避免频繁 TLS 全握手（5-8s 高功耗） */
#define MQTT_RECONNECT_TIMEOUT_MS 60000

/* 主循环 MQTT 监督（esp-mqtt 内部 auto-reconnect 静默失效后无人兜底）：库内重连不抛事件，
 * TCP 半开/陈旧 socket/TLS、DNS 异常卡死时收不到任何信号。PERIOD = 监督唤醒周期（耗电相比
 * DTIM 唤醒可忽略）；STUCK_DEADLINE = WiFi 在线且 MQTT 连续断连超此时长 → 强制一次 token-reuse
 * 恢复（毁旧建新 = 全新 TCP/TLS/DNS + 重新订阅），须大于库内自然重试周期（reconnect_timeout
 * 60s + 裕量）；连续强制仍失败则间隔翻倍至 MAX，连接成功归位。CONNACK 拒绝走全量重认证。 */
#define MQTT_SUPERVISE_PERIOD_MS   30000U
#define MQTT_STUCK_DEADLINE_MS     180000U
#define MQTT_SUPERVISE_MAX_MS      900000U

/* 工厂复位引脚 —— 上电时该引脚被拉低 = 清空全部持久化参数并重启（现场换网/换平台/
 * 参数被改坏的本地兜底，无需串口工具）。app_main 最早期（nvs_flash_init 之前）检测：
 * 引脚输入 + 内部上拉，悬空 = 高 = 不复位；采样为低 → nvs_flash_erase() 抹掉整个 NVS
 * 分区 → esp_restart()。被清除：WiFi 凭据与已配网标记（namespace "wifi"/"provisioned"
 * 及 WiFi 驱动自身命名空间）、设备 Token（"device"/"auth_token"）、appcfg 配置快照与
 * 版本（"appcfg"/version·payload·rpending）；墨水屏图片在独立 FAT 分区（storage）不受
 * 影响；重启后回 SmartConfig 配网（见 wifi.c）。引脚持续拉低会反复擦除重启，松开即正常启动。
 * 引脚选择（ESP32-S3，选错会导致无法启动或复位失效）：
 *   禁 GPIO0/3/45/46 strapping 引脚：其中 GPIO0 为低直接进串口下载模式，应用根本不运行；
 *   禁 GPIO19/20 USB-Serial-JTAG 副控制台（sdkconfig 已启用）；禁 GPIO26~32 片外 SPI
 *   Flash（16MB）；避 GPIO43/44 UART0 主控制台；GPIO7~12 为板载墨水屏（见 periph/epd.h）。
 * 默认取 4：以上均未占用；启动后由 periph_pin_claim() 占位，云端物模型不得分配此引脚。 */
#define FACTORY_RESET_GPIO         4

#endif /* _CONFIG_H_ */
