# net 组件：平台接入层 = REST 通道（Token 获取 / 命令队列兜底 / 统一信封解析）
# + 设备 Token NVS 存取（token.c，认证子模块）。
idf_component_register(
    SRCS "src/network.c" "src/token.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES
        core
        espressif__cjson
        esp_http_client
        mbedtls
        nvs_flash
)
