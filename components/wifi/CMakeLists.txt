# wifi 组件：STA 连接/自动重连 + SmartConfig 配网。
idf_component_register(
    SRCS "wifi.c"
    INCLUDE_DIRS "include"
    REQUIRES
        core
        esp_wifi
        esp_netif
        esp_event
        wpa_supplicant
        nvs_flash
)