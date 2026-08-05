idf_component_register(
    SRC_DIRS "."
             "peripherals"
             "peripherals/drivers"
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
        led_strip
    INCLUDE_DIRS "."
                 "peripherals"
                 "peripherals/drivers"
)
