#ifndef _DISPLAY_DRIVER_H_
#define _DISPLAY_DRIVER_H_

#include "periph.h"

/* epd transport — 板载显示器（当前 SSD1683 4.2" BWR）。
 * config：{"transport":"epd"} + 可选 width/height/chip/color_depth 校验；
 * 引脚见 display.h，控制命令 {"op":"selftest"}。 */

extern periph_driver_t display_driver;

#endif /* _DISPLAY_DRIVER_H_ */
