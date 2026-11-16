# display 组件：面板无关的显示层 + 控制器适配层。
# 公共头：display.h(上层 API + 逻辑位图格式)、display_ctrl.h(适配层接口)。
# 新增面板：在 ctrl/ 下实现 display_ctrl_t，并在 display.c 的 s_ctrls 里登记。
idf_component_register(
    SRCS
        "display.c"
        "ctrl/ssd1683.c"
        "ctrl/jd79665.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_driver_gpio
        esp_driver_spi
        esp_timer
        esp_pm
)
