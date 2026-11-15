# periph 组件：外设总线 + 执行器驱动（gpio/pwm/spi/led_strip）+ 板载显示器 transport 胶水层。
# 公共头：periph.h / ledc_pool.h；驱动私有头位于 src/drivers/（PRIV_INCLUDE_DIRS）。
idf_component_register(
    SRCS
        "src/periph.c"
        "src/ledc_pool.c"
        "src/drivers/gpio_driver.c"
        "src/drivers/pwm_driver.c"
        "src/drivers/spi_driver.c"
        "src/drivers/led_strip_driver.c"
        "src/drivers/display_driver.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
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
