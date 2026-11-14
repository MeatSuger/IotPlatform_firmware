idf_component_register(
    SRCS "src/mqtt_app.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES
        core
        net
        img
        espressif__mqtt
        mbedtls
)
