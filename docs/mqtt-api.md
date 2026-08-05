# MQTT API 文档

## 连接信息

| 项目 | 值 |
|---|---|
| Broker (WSS) | `wss://api.meatsuger.top/api/ws/mqtt/broker` |
| 用户名 | `90431b`（DEVICE_ID） |
| 密码 | 登录接口获取的 auth token |
| 订阅（命令） | `device/90431b/cmd` |
| 发布（响应） | `device/90431b/resp` |

## 消息信封（所有命令统一格式）

```json
{ "deviceId": "90431b", "type": "<register|control>", "payload": { ... } }
```

- `deviceId`：必填，必须等于本机 `90431b`，否则丢弃
- `type`：必填，只能是 `register`（设备注册）或 `control`（设备控制），其他值丢弃
- `payload`：命令内容

## 响应格式（发布到 resp 主题）

```json
{ "type": "response", "status": "ok" | "skipped" }
```

- `ok`：命令执行成功
- `skipped`：未识别 / 参数错误 / 设备不存在 / 执行失败
- JSON 解析失败时响应 `{"type":"response","status":"parse_error"}`

---

## 一、设备注册（type: "register"）

设备配置持久化到 NVS，**重启自动恢复，只需注册一次**。

### 注册设备

```json
{
  "deviceId": "90431b",
  "type": "register",
  "payload": {
    "action": "config",
    "device": "设备名",      // 自定义，控制命令用；支持多实例 led0/led1
    "driver": "led",         // 驱动名：led / servo / speaker
    "config": { }            // 设备参数，省略的字段用默认值
  }
}
```

各驱动的 `config` 字段：

| 驱动 | 字段 | 默认值 | 说明 |
|---|---|---|---|
| `led` | `gpio` | 48 | WS2812 数据引脚 |
| | `count` | 1 | LED 数量 |
| `servo` | `gpio` | 18 | 信号引脚 |
| | `min_pulse_us` | 500 | 最小脉宽 |
| | `max_pulse_us` | 2500 | 最大脉宽 |
| | `min_angle` | 0 | 最小角度 |
| | `max_angle` | 180 | 最大角度 |
| `speaker` | `gpio` | 2 | 引脚 |
| | `type` | `"active"` | `"active"` 有源 / `"passive"` 无源 |

示例：

```json
{"deviceId":"90431b","type":"register","payload":{"action":"config","device":"led","driver":"led","config":{"gpio":48,"count":1}}}
{"deviceId":"90431b","type":"register","payload":{"action":"config","device":"servo","driver":"servo","config":{"gpio":18,"min_pulse_us":500,"max_pulse_us":2500,"min_angle":0,"max_angle":180}}}
{"deviceId":"90431b","type":"register","payload":{"action":"config","device":"speaker","driver":"speaker","config":{"gpio":2,"type":"active"}}}
```

重新下发同一设备名 = 重新配置（先卸载再加载，立即生效）。

### 注销设备

```json
{"deviceId":"90431b","type":"register","payload":{"action":"unconfig","device":"led"}}
```

---

## 二、设备控制（type: "control"）

`action` = 已注册的设备名，路由到该设备的驱动。

### LED（板载单颗 RGB）

```json
// 设置颜色（r/g/b：0-255，越界自动截断）
{"deviceId":"90431b","type":"control","payload":{"action":"led","value":{"r":255,"g":0,"b":0}}}
```

### 舵机

```json
// 按角度（映射 min_angle~max_angle → min_pulse_us~max_pulse_us）
{"deviceId":"90431b","type":"control","payload":{"action":"servo","value":{"angle":90}}}
// 直接指定脉宽（us，自动截断到配置范围）
{"deviceId":"90431b","type":"control","payload":{"action":"servo","value":{"pulse_us":1500}}}
```

### 蜂鸣器

```json
// 有源：开关
{"deviceId":"90431b","type":"control","payload":{"action":"speaker","value":{"state":"on"}}}
{"deviceId":"90431b","type":"control","payload":{"action":"speaker","value":{"state":"off"}}}
// 无源：频率音调（100-20000 Hz），duration_ms 可选，到时自动关
{"deviceId":"90431b","type":"control","payload":{"action":"speaker","value":{"freq":1000,"duration_ms":200}}}
```

---

## 三、遗留 GPIO 命令（type: "control"，免注册）

```json
{"deviceId":"90431b","type":"control","payload":{"GPIO":"48","action":"toggle"}}
{"deviceId":"90431b","type":"control","payload":{"GPIO":"2","action":"on"}}
{"deviceId":"90431b","type":"control","payload":{"GPIO":"2","action":"off"}}
{"deviceId":"90431b","type":"control","payload":{"GPIO":"2","action":"pwm","value":80}}
```

| action | 说明 |
|---|---|
| `on` / `off` | GPIO 高/低电平 |
| `toggle` | 翻转电平 |
| `pwm` | LEDC PWM，`value` 0-100（5000 Hz） |

`GPIO` 为字符串数字，如 `"48"`。

---

## 完整流程示例

1. 开机 → 已注册设备自动从 NVS 恢复，可直接控制
2. 首次部署：注册设备（一次）
3. 日常控制：控制命令
4. 换硬件/改参数：重新下发 config；不再使用：unconfig

```json
// 注册 LED
{"deviceId":"90431b","type":"register","payload":{"action":"config","device":"led","driver":"led","config":{"gpio":48,"count":1}}}
// 响应：{"type":"response","status":"ok"}

// 点亮
{"deviceId":"90431b","type":"control","payload":{"action":"led","value":{"r":255,"g":0,"b":0}}}
// 响应：{"type":"response","status":"ok"}
```
