idf_component_register(
    SRCS "src/appcfg.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES
        core
        mqtt_app
        periph
        espressif__cjson
        nvs_flash
)
