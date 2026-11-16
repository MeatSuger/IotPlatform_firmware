# periph 组件：外设总线 + 执行器驱动(gpio/pwm/spi/led_strip)+ 板载显示器 transport 胶水层。
# 头文件：periph.h / ledc_pool.h，驱动实现细节头位于 include/drivers/。
idf_component_register(
    SRCS
        "periph.c"
        "ledc_pool.c"
        "drivers/gpio_driver.c"
        "drivers/pwm_driver.c"
        "drivers/spi_driver.c"
        "drivers/led_strip_driver.c"
        "drivers/display_driver.c"
    INCLUDE_DIRS "include"
    REQUIRES
        core
        display
        espressif__cjson
        espressif__led_strip
        esp_driver_gpio
        esp_driver_ledc
        esp_driver_spi
        esp_timer
)
