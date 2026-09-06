# core 组件：共享基础设施（common.h 调试宏/全局状态声明 + config.h 编译期常量）。
# 纯头组件（无源文件）；头引用的 IDF 组件在 REQUIRES 中声明，随依赖传递给使用方。
# 注：MQTT 句柄等类型依赖已收敛到 mqtt 组件（mqtt_app.h），core 不引入 managed 依赖。
idf_component_register(
    INCLUDE_DIRS "include"
    REQUIRES
        nvs_flash
        esp_wifi
        esp_event
)
