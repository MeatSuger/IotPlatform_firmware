# IoT Platform Firmware

ESP32-S3 设备固件,基于 ESP-IDF 构建,支持 WiFi 联网、MQTT 双向通信、传感器数据采集与上报。

## 技术栈

| 组件 | 说明 |
|------|------|
| MCU | ESP32-S3 |
| SDK | ESP-IDF v6.0 |
| 构建 | CMake |
| 配网 | SmartConfig(ESP-Touch / AirKiss) |
| MQTT | espressif/mqtt |
| JSON | cJSON |
| LED | led_strip (WS2812) |

## 功能

- **SmartConfig 配网**：首次上电(NVS 无“已配网”标记)进入配网模式；手机连上
  目标 2.4G WiFi 后用 ESP-Touch / AirKiss 应用广播 SSID/密码即可完成配网,凭据
  自动持久化,后续上电直连(重新配网：上电拉低 `GPIO4` 工厂复位,或
  `idf.py -p PORT erase-flash`)
- **设备引导密钥**：首次取 Token 需 `DEVICE_SECRET`(注册设备时返回一次,写入本地
  `components/core/include/secrets.h`,该文件不入库)；仅凭设备 ID 不能换取凭证。
  NVS 中已有 Token 时复用,Token 失效/被吊销才用密钥重新认证
- **MQTT 话题隔离**：设备连接只能发布/订阅 `iot/{自身ID}/...`；`iot/#`、跨设备
  话题、`$SYS/#` 在网关层被丢弃(固件应只订阅自身配置/命令/图片话题)
- **工厂复位**：上电时 `GPIO4`(内部上拉,悬空不复位)被拉低 → 擦除整个 NVS 分区
  (WiFi 凭据与已配网标记、设备 Token、云端配置快照/版本/待回执标志)并自动重启
  回到配网流程；墨水屏图片存于独立 FAT 分区不受影响。引脚定义与选型约束见
  `components/core/include/config.h`(不得选 strapping 引脚 GPIO0/3/45/46,否则
  拉低会改变启动行为)
- WiFi 自动重连(已配网后)
- MQTT 连接 IoT 平台(设备注册 / 数据上报 / 下行命令接收)
- 传感器数据采集(温度等)
- LED 状态指示
- **OTA 升级**：固件内置固定 HTTPS 地址(`config.h` 的 `OTA_FW_URL`),开机联网后
  检查镜像头版本,有新版本才下载并重启;不经 MQTT,也不读云端 `config.ota`
- Token 管理

## 模块结构(ESP-IDF 组件化布局,见 https://docs.espressif.com/projects/esp-idf/zh_CN/latest/esp32s3/api-guides/build-system.html )

```
firmware/
├── CMakeLists.txt              # 顶层 CMake(ESP-IDF 项目)
├── main/                       # 应用入口组件(仅 app_main 装配与全局状态)
│   ├── CMakeLists.txt
│   ├── idf_component.yml       # 第三方依赖清单(espressif/mqtt·cjson·led_strip)
│   └── app_main.c
├── components/                 # 业务组件(每组件 = 独立 CMakeLists + include/ 头文件 + 同级 .c 实现)
│   ├── core/                   # 共享基础设施(common.h 调试宏/全局声明 + config.h 常量,纯头组件)
│   ├── wifi/                   # WiFi 连接管理
│   ├── net/                    # 平台接入层(HTTP:Token 获取/命令兑底/信封解析 + Token NVS 存取)
│   ├── mqtt_app/               # MQTT 客户端(WSS 连接/订阅下行/遥测发布)+ 认证拒绝检测
│   ├── appcfg/                 # 配置快照状态机(NVS 持久化/version 幂等/回执/重启重放)
│   ├── periph/                 # 声明式外设总线(device/driver 模型)：按执行器定义
│   │                           #   config.transport(gpio/pwm/spi/led_strip) 实例化,引脚仲裁
│   ├── display/                # 显示层：面板无关 API(display.h)+ 控制器适配(display_ctrl.h)
│   │                           #   ctrl/ 下按面板拆分：ssd1683.c(4.2" BWR)、
│   │                           #   jd79665.c(3.98" BWRY)；开机按 BUSY 极性自动识别
│   ├── img/                    # 图片接收：控制帧 + 二进制分片 → FatFS(/fatfs) → 刷屏
│   ├── ota/                    # 独立 HTTPS 升级(固定 URL,开机检查镜像版本→写入→重启)
│   └── app/                    # 应用运行时(周期上报/命令分发任务 + 传感器按 type 采集器上报)
├── managed_components/         # 第三方组件(component manager 自动拉取,勿手改)
├── dependencies.lock           # 组件版本锁定
├── partitions.csv              # 分区表
├── sdkconfig.defaults          # SDK 默认配置
└── docs/                       # 协议文档
```

> 组件依赖通过各组件 CMakeLists.txt 的 `REQUIRES` 显式声明(`espressif__*` 为
> managed 组件全名)；组件头文件统一放 `include/`(子模块可用子目录),`.c` 与 `include/` 同级。

## 快速开始

### 前置条件

- ESP-IDF v6.0+([安装指南](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/index.html))
- ESP32-S3 开发板
- USB 转串口驱动

### 构建与烧录

```bash
cd firmware

# 配置本地引导密钥(每个开发环境一次；secrets.h 已被 .gitignore 忽略)
cp components/core/include/secrets.h.example components/core/include/secrets.h
# 编辑 secrets.h,把 DEVICE_SECRET 换成注册设备时返回的 deviceSecret

# 设置目标芯片
idf.py set-target esp32s3

# 编译
idf.py build

# 烧录(替换 /dev/ttyUSB0 为实际串口)
idf.py -p /dev/ttyUSB0 flash

# 串口监视
idf.py -p /dev/ttyUSB0 monitor
```

### 配置

编译前确认：

- `components/core/include/config.h`：`DEVICE_ID`(注册响应中的 `deviceId`)、
  `API_HOST` / `DEVICE_UPDATE_URL` / `MQTT_BROKER_PATH`(平台域名与接入路径)、
  `OTA_FW_URL`(固件镜像固定地址,空串关闭 OTA)
- `components/core/include/secrets.h`：`DEVICE_SECRET`(注册响应中的 `deviceSecret`,
  仅返回一次；换设备/重置后需同步更新)。该文件已在 `.gitignore` 中,不要提交。
  若密钥曾进入版本库,需在平台侧轮换(`POST /api/devices/{deviceId}/secret`)

WiFi 不预置 SSID/密码：首次上电进入 SmartConfig 配网,凭据由手机广播并持久化到 NVS。

## 依赖组件

| 组件 | 版本 | 用途 |
|------|------|------|
| `espressif/mqtt` | 1.1.0 | MQTT 客户端 |
| `espressif/cjson` | 1.7.19 | JSON 解析 |
| `espressif/led_strip` | 3.0.3 | WS2812 LED 驱动 |

## License

MIT
