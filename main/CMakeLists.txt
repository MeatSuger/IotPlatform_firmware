# main 组件：应用入口（app_main）与全局装配。
# 业务模块已按 ESP-IDF 推荐结构拆分为独立组件（components/ 下），
# 每个组件自带 CMakeLists.txt（源文件 + 公共头 + REQUIRES 依赖声明）。
idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES
        app
        appcfg
        core
        mqtt_app
        net
        periph
        wifi
)
