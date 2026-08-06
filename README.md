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

## 模块结构

```
firmware/
├── main/
│   ├── app/             # 应用主逻辑
│   ├── wifi/            # WiFi 连接管理
│   ├── mqtt/            # MQTT 客户端（发布/订阅/下行命令）
│   ├── sensor/          # 传感器驱动与采集
│   ├── net/             # HTTP 网络请求（设备注册、Token 获取）
│   ├── token/           # Token 存储与认证
│   ├── core/            # 核心任务调度
│   └── peripherals/     # 外设驱动（LED 等）
├── CMakeLists.txt       # 顶层 CMake（ESP-IDF 项目）
├── dependencies.lock    # IDF 组件版本锁定
├── partitions.csv       # 分区表
├── sdkconfig.defaults   # SDK 默认配置
├── certs/               # TLS 证书
└── docs/                # 文档
```

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
