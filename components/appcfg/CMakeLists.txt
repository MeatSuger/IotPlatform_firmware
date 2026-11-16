idf_component_register(
    SRCS "appcfg.c" "appcfg_logic.c"
    INCLUDE_DIRS "include"
    REQUIRES
        core
        mqtt_app
        periph
        espressif__cjson
        nvs_flash
)
