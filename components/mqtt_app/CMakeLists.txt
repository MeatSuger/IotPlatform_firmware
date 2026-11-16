idf_component_register(
    SRCS "mqtt_app.c"
    INCLUDE_DIRS "include"
    REQUIRES
        core
        net
        img
        espressif__mqtt
        mbedtls
)
