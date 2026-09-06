idf_component_register(
    SRCS "src/wifi.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES
        core
        esp_wifi
        esp_netif
        esp_event
        wpa_supplicant
)
