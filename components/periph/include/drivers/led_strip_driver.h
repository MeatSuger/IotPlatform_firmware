#ifndef _LED_STRIP_DRIVER_H_
#define _LED_STRIP_DRIVER_H_

#include "periph.h"

/* LED strip transport — WS2812 灯带(espressif/led_strip over RMT)。
 * PERIPH_LED_ENABLE 为编译开关(默认 1，置 0 整体编出，用于无 WS2812 的板子)；
 * WS2812_GPIO / WS2812_LED_COUNT 为 config 缺省时的默认值。
 * Device config：{"transport":"led_strip","specs":{"gpio":48,"count":1}}
 * Control command：{"action":"led1","value":{"rgb":[255,0,0]}} — rgb 作用于全部灯珠。
 */

#ifndef PERIPH_LED_ENABLE
#define PERIPH_LED_ENABLE 1
#endif

#ifndef WS2812_GPIO
#define WS2812_GPIO 48   /* ESP32-S3-DevKitC-1 on-board WS2812 */
#endif

#ifndef WS2812_LED_COUNT
#define WS2812_LED_COUNT 1
#endif

extern periph_driver_t led_strip_driver;

#endif /* _LED_STRIP_DRIVER_H_ */
