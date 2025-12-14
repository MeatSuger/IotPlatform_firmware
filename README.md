# Firmware (PlatformIO)

设备固件项目，基于 PlatformIO平台，主板为ESP32-dev。

项目结构

- `platformio.ini`：平台与依赖配置
- `src/main.cpp`：主程序入口
- `include/`、`lib/`：头文件与库
- `test/`：单元测试（如需）

环境要求

- 安装 PlatformIO CLI 或 VS Code + PlatformIO 插件
- 已正确选择目标开发板（见 `platformio.ini` 的 `env` 配置）

常用命令（Windows PowerShell）

```powershell
cd .\firmware
pio --version
pio run                # 编译
pio run -t upload     # 烧录（连接开发板）
pio device monitor    # 串口监视
```

开发提示

- 根据实际硬件修改 `platformio.ini` 的 `board`、`framework`、端口等配置
- 在 `src/main.cpp` 中实现传感读取、通信协议（如串口、MQTT、LoRa、BLE 等）
- 若与后端对接，请统一数据格式（JSON/二进制）并约定主题与路由

故障排查

- 烧录失败：检查串口占用、USB线、驱动与权限
- 编译错误：确认库版本与 C++ 标准配置，清理 `pio run -t clean`
- 监视无输出：检查 `Serial.begin(baud)` 与端口选择
