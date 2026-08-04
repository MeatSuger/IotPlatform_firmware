idf_component_register(
    SRCS "main.c"
         "network.c"
         "sensor.c"
         "task.c"
         "token.c"
    REQUIRES
        driver
        esp_driver_gpio
        esp_driver_ledc
        esp_driver_tsens
        esp_http_client
        esp_netif
        esp-tls
        esp_event
        esp_wifi
        nvs_flash
        wpa_supplicant
    INCLUDE_DIRS "."
)
