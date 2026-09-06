# IoT Platform Firmware

ESP32-S3 设备固件，基于 ESP-IDF 构建，支持 WiFi 联网、MQTT 双向通信、传感器数据采集与上报。

## 技术栈

| 组件 | 说明 |
|------|------|
| MCU | ESP32-S3 |
| SDK | ESP-IDF v6.0 |
| 构建 | CMake |
| MQTT | espressif/mqtt |
| JSON | cJSON |
| LED | led_strip (WS2812) |

## 功能

- WiFi 配网与自动重连
- MQTT 连接 IoT 平台（设备注册 / 数据上报 / 下行命令接收）
- 传感器数据采集（温度等）
- LED 状态指示
- OTA / Token 管理

## 模块结构（ESP-IDF 组件化布局，见 https://docs.espressif.com/projects/esp-idf/zh_CN/latest/esp32s3/api-guides/build-system.html ）

```
firmware/
├── CMakeLists.txt              # 顶层 CMake（ESP-IDF 项目）
├── main/                       # 应用入口组件（仅 app_main 装配与全局状态）
│   ├── CMakeLists.txt
│   ├── idf_component.yml       # 第三方依赖清单（espressif/mqtt·cjson·led_strip）
│   └── app_main.c
├── components/                 # 业务组件（每组件 = 独立 CMakeLists + include/ 公共头 + src/ 实现）
│   ├── core/                   # 共享基础设施（common.h 调试宏/全局声明 + config.h 常量，纯头组件）
│   ├── wifi/                   # WiFi 连接管理
│   ├── net/                    # 平台接入层（HTTP:Token 获取/命令兑底/信封解析 + Token NVS 存取）
│   ├── mqtt_app/               # MQTT 客户端（WSS 连接/订阅下行/遥测发布）+ 认证拒绝检测
│   ├── appcfg/                 # 配置快照状态机（NVS 持久化/version 幂等/回执/重启重放）
│   ├── periph/                 # 声明式外设总线（device/driver 模型）：按执行器定义
│   │                           #   config.transport(gpio/pwm/spi/led_strip) 实例化，引脚仲裁
│   └── app/                    # 应用运行时（周期上报/命令分发任务 + 传感器按 type 采集器上报）
├── managed_components/         # 第三方组件（component manager 自动拉取，勿手改）
├── dependencies.lock           # 组件版本锁定
├── partitions.csv              # 分区表
├── sdkconfig.defaults          # SDK 默认配置
└── docs/                       # 协议文档
```

> 组件依赖通过各组件 CMakeLists.txt 的 `REQUIRES` 显式声明（`espressif__*` 为
> managed 组件全名）；组件公共头放 `include/`，实现与私有头放 `src/`。

## 快速开始

### 前置条件

- ESP-IDF v6.0+（[安装指南](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/index.html)）
- ESP32-S3 开发板
- USB 转串口驱动

### 构建与烧录

```bash
cd firmware

# 设置目标芯片
idf.py set-target esp32s3

# 编译
idf.py build

# 烧录（替换 /dev/ttyUSB0 为实际串口）
idf.py -p /dev/ttyUSB0 flash

# 串口监视
idf.py -p /dev/ttyUSB0 monitor
```

### 配置

在 `sdkconfig.defaults` 或通过 `idf.py menuconfig` 设置：

- WiFi SSID / 密码
- MQTT Broker 地址
- 设备注册服务器 URL

## 依赖组件

| 组件 | 版本 | 用途 |
|------|------|------|
| `espressif/mqtt` | 1.1.0 | MQTT 客户端 |
| `espressif/cjson` | 1.7.19 | JSON 解析 |
| `espressif/led_strip` | 3.0.3 | WS2812 LED 驱动 |

## License

MIT
