#ifndef _GPIO_DRIVER_H_
#define _GPIO_DRIVER_H_

#include "periph.h"

/* --------------------------------------------------------------------------
 * GPIO transport — 数字输出（继电器/单色 LED/电平信号）。
 *
 * Device config:
 *   {"transport":"gpio","pin":4,"active_high":true,"initial":0}
 *     pin         必填，GPIO 编号
 *     active_high 逻辑 1 对应物理高电平（默认 true）
 *     initial     probe 时的初始逻辑电平 0/1（默认 0）
 *
 * Control command (action = device name, value 原语)：
 *   {"action":"relay1","value":{"level":1}}   — 置逻辑电平
 *   {"action":"relay1","value":{"toggle":true}} — 翻转
 * -------------------------------------------------------------------------- */

extern periph_driver_t gpio_driver;

#endif /* _GPIO_DRIVER_H_ */
