# ota 组件：独立 HTTPS 固件升级(固定 URL，开机检查镜像版本)。
idf_component_register(
    SRCS "ota.c"
    INCLUDE_DIRS "include"
    REQUIRES
        bootloader_support
        core
        esp_https_ota
        freertos
        mbedtls
)
