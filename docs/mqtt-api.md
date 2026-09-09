# MQTT API 文档（终端设备）

## 连接信息

| 项目 | 值 |
|---|---|
| Broker (WSS) | `wss://api.meatsuger.top/api/ws/mqtt/broker` |
| 用户名 | `90431b`（DEVICE_ID） |
| 密码 | 设备 Token（`GET /api/devices/{deviceId}/token` 获取） |
| 订阅（命令） | `iot/90431b/cmd`（QoS1） |
| 订阅（配置快照） | `iot/90431b/config`（QoS1 + retained） |
| 发布（配置回执） | `iot/90431b/config/report`（QoS1） |
| 发布（遥测） | `iot/90431b/telemetry` |

## 设计原则

设备端**不做器件语义假设**，一切由云端下发的物模型定义驱动：

- **写入类设备**（继电器/风扇/舵机/灯带/DAC 等）→ 云端 `actuators[]` 定义，固件按其
  `specs.transport`（`gpio` / `pwm` / `spi` / `led_strip`）实例化硬件；控制命令
  `value` 下发**传输原语**（电平/占空比/脉宽/数据帧/颜色分量），角度、颜色等语义
  换算由云端完成。
- **读取类设备**（温度/湿度等传感器）→ 云端 `sensors[]` 定义，固件按其 `type`
  匹配采集器读取并上报；`type` 无枚举限制（temperature/humidity/...）。
- 后端 Actuator 的 `driver` 字段枚举固定（led/servo/speaker），固件**忽略该字段**，
  仅作为后端占位；设备类型一律以 `specs.transport` 为准。
- 命令无 ack 回执：控制为 fire-and-forget，最终状态经遥测/配置回执体现。

## 协议总览

设备状态 = 云端配置快照（唯一真源），动作 = 下行命令：

| 通道 | 方向 | 内容 |
|---|---|---|
| `iot/{id}/config` | 云端 → 设备 | 整体配置快照（retained，**订阅即拉取**；含执行器定义 `actuators`） |
| `iot/{id}/config/report` | 设备 → 云端 | 配置回执（应用成功后，云端回写 `status=acked`） |
| `iot/{id}/cmd` | 云端 → 设备 | 实时命令（控制执行器；离线由 HTTP `GET /commands` 兜底） |
| `iot/{id}/telemetry` | 设备 → 云端 | 传感器遥测（`{"sensors":[...]}`，网关自动入库） |

---

## 一、配置快照（topic: `iot/{id}/config`，retained）

平台 `POST /api/devices/{deviceId}/config` 或 `POST /sensors/apply` / `POST /actuators/apply`
保存后，以 QoS1 + retained 发布最新快照：

```json
{"version": 4, "config": { "sensor": {...}, "sensors": [...], "actuators": [...], ... }}
```

- `version`：递增的配置代数（云端按每次保存 +1）
- `config.sensors`：传感器定义数组（物模型，固件按 type 采集上报）
- `config.actuators`：**执行器定义数组（期望列表，唯一真源）**
- `config.sensor.reportInterval`（秒）：传感器上报周期（默认 30s，范围 1–86400）
- 其余分区（`network`/`camera`/`ota` 等）：设备当前不生效，但随 payload 原样持久化与回执

### 执行器定义（actuators[]）— transport 由 specs 自描述

> **字段名统一（2026）**：后端驱动参数已由 `config` 改名为 `specs`
> （与 Sensor 定义体同名，DB 列 params→specs）；下发为**裁剪版**
> （仅 `id/driver/specs/enabled`，无 `name/createdAt/updatedAt`）；空 specs 省略
> （无 transport 无法实例化，等价移除）。

```json
{"id":"fan1","driver":"servo","enabled":true,
 "specs":{"transport":"pwm","pin":18,"freq_hz":25000}}
```

| 字段 | 说明 |
|---|---|
| `id` | 执行器标识符（`^[a-z][a-z0-9_]{0,10}$`）＝ 控制命令 `action` |
| `driver` | 后端枚举占位（led/servo/speaker 三选一即可），**固件忽略** |
| `specs.transport` | 设备类型：`gpio` / `pwm` / `spi` / `led_strip`（固件注册表，可扩展） |
| `specs.*` | transport 参数（见下） |
| `enabled` | `false`（或缺省 true）→ 期望移除/不实例化 |

固件处理规则（appcfg + periph diff 应用）：

1. 新版本 → NVS 持久化（version/payload/rpending）→ 逐条 diff：
   - 新增 → probe 实例化（按 specs.transport 匹配驱动，引脚仲裁防冲突）
   - transport/参数变化 → 重配置（remove 旧绑定 → 重新 probe）
   - `enabled=false`、specs 非对象或缺失 `transport` → 移除
2. 已应用版本重投（retained/重连）→ 幂等忽略；回执未发出则补发
3. 重启 → `appcfg_replay()` 从 NVS 重放（不等云端推送）

各 transport 参数与命令原语：

| transport | specs 参数 | 控制命令 value | 说明 |
|---|---|---|---|
| `gpio` | `pin` 必填；`active_high` 默认 true；`initial` 默认 0 | `{"level":1}` / `{"level":0}` / `{"toggle":true}` | 数字输出；逻辑电平按 active_high 映射物理电平 |
| `pwm` | `pin` 必填；`freq_hz` 默认 1000 | `{"duty":80}`（0-100%）/ `{"pulse_us":1500}` | LEDC 输出；通道按引脚自动分配、timer 按频率共享；分辨率按频率自动推导 |
| `spi` | `clk`/`mosi`/`cs` 必填；`miso` 可选；`freq_hz` 默认 1MHz；`mode` 默认 0 | `{"tx":"A5 3C FF"}` 或 `{"tx":[165,60,255]}` | SPI 主机写；同 host 总线引脚由首实例初始化、多设备独立 CS |
| `led_strip` | `gpio` 默认 48；`count` 默认 1（可编译关闭） | `{"rgb":[255,0,0]}` | WS2812 灯带；rgb 原语作用于全部灯珠 |

> 角度/颜色等**器件语义由云端换算成原语后下发**：舵机类 → `pulse_us`，调色 → `rgb`。

### 回执（topic: `iot/{id}/config/report`）

应用成功后发布（config 为**接收到的完整 config 原文**，含未生效字段，便于云端“期望 vs 实际”比对）：

```json
{"version": 4, "config": { ... }}
```

回执发布失败会保留本地 `rpending` 标志：重连（retained 重投）或周期任务自动补发；
云端收到后回写 `reportedVersion/reportedPayload` 并置 `status=acked`。

---

## 二、下行命令（topic: `iot/{id}/cmd`，QoS1）

命令项与 HTTP `GET /commands` 返回项同构：

```json
{"id": 42, "type": "control", "payload": {"action":"fan1","value":{...}}, "createdAt": "..."}
```

| `type` | `payload` | 处理 |
|---|---|---|
| `config` | `{"version":N,"config":{...}}` | 与 retained 快照同解析（appcfg 状态机） |
| `control` | `{"action":<执行器id>,"value":{...}}` | 路由到对应 transport 执行 |

控制示例：

```json
{"id":42,"type":"control","payload":{"action":"fan1","value":{"duty":80}}}          // pwm 风扇
{"id":42,"type":"control","payload":{"action":"servo1","value":{"pulse_us":1500}}}   // pwm 舵机(云端换算)
{"id":42,"type":"control","payload":{"action":"relay1","value":{"level":1}}}        // gpio 继电器
{"id":42,"type":"control","payload":{"action":"dac1","value":{"tx":"A5 3C FF"}}}    // spi 写
{"id":42,"type":"control","payload":{"action":"led1","value":{"rgb":[255,0,0]}}}    // led_strip
```

未知 action / value 格式错误：固件丢弃并打日志（无回执，见设计原则）。

---

## 三、遥测上报（topic: `iot/{id}/telemetry`）

按 `config.sensor.reportInterval`（秒）周期发布。**上报内容由云端 `sensors[]` 定义驱动**：

> 下发为**裁剪版**（仅 `id/type/dataType/unit/specs/reportInterval/enabled`，
> 无 `name/createdAt/updatedAt`；空 specs / 继承全局周期的 reportInterval 省略）。
> 传感器级 `specs` 为统一定义体：量程 `min/max/step`、枚举 `values`、文本 `maxLen`、
> 告警阈值 `thresholds{min,max,…}` 与自由扩展键平铺于同一对象（原顶层
> `thresholds`/`attrs` 已并入）。固件采集器仅需 `id/type/enabled/dataType`。

> **上报值类型对齐**：平台按定义 `dataType` 校验上报值——`float/int` 必须为 number、
> `bool` 必须为 true/false、`text`/`enum` 必须为字符串（enum 还须在 `specs.values`
> 内）；类型不符的数据点会被平台**丢弃**。固件采集器产物须与 dataType 匹配
> （sensor.c 已有源头自检），例如 bool 开关要产出 `true/false` 而非 0/1。

```json
// 云端定义（下发裁剪版）: {"sensors":[{"id":"temp1","type":"temperature","dataType":"float","enabled":true}]}
// 设备上报:
{"sensors":[{"name":"temp1","type":"temperature","value":26.32}]}
```

| 规则 | 说明 |
|---|---|
| `sensors[].name` | = 定义 `id`（平台据此与服务端物模型定义 join） |
| `sensors[].type` | = 定义 `type`（后端无枚举限制；temperature/humidity/...） |
| `value` | 由匹配 type 的固件采集器产出（number/string/bool） |
| 无定义/全失败 | 该轮不上报（平台详情页显示"无数据"） |
| 未知 type | 固件告警跳过该定义（采集器注册表可扩展：新增读类器件时注册新采集器） |

内置采集器：`temperature`（ESP32 内部温度，两位小数）。其余读类器件
（I2C/SPI 传感器等）按需在固件注册新采集器，云端无需改动。

---

## 完整流程示例

1. 平台：`POST /actuators` 创建执行器定义（driver 占位，specs.transport=pwm → 风扇）
2. 平台：`POST /actuators/apply` → version+1 配置下发（MQTT retained + 命令队列）
3. 设备：订阅即收到 → NVS 持久化 → periph 按 transport 实例化 → 回执 `config/report` → 云端 `acked`
4. 平台：`POST /sensors/apply` → 设备按 type=temperature 采集并周期上报遥测
5. 平台：`POST /commands`（type=control，action=fan1，value.duty）→ MQTT 实时到达 → PWM 输出
6. 设备重启：NVS 重放执行器定义与上报周期，不等云端推送即恢复
7. 配置变更：再次 Apply（新版本）→ 设备 diff 重配置；删除定义 → 设备卸载
