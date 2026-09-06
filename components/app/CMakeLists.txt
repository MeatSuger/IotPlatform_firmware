# app 组件：应用运行时 = 周期上报/命令分发任务（app.c）+
# 传感器采集与遥测 JSON 构建（sensor.c，内置温度传感器）。
idf_component_register(
    SRCS "src/app.c" "src/sensor.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES
        core
        appcfg
        mqtt_app
        net
        periph
        espressif__cjson
        esp_driver_tsens
)
