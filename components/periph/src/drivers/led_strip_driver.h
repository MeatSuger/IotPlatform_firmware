#ifndef _LED_STRIP_DRIVER_H_
#define _LED_STRIP_DRIVER_H_

#include "periph.h"

/* --------------------------------------------------------------------------
 * LED strip transport — WS2812 灯带（espressif/led_strip over RMT）。
 *
 * Compatibility macros:
 *   PERIPH_LED_ENABLE — compile switch (default 1). Set to 0 to compile
 *                       the driver out entirely (board without WS2812).
 *   WS2812_GPIO       — default data pin when config omits "gpio".
 *   WS2812_LED_COUNT  — default LED count when config omits "count".
 *
 * Device config:
 *   {"transport":"led_strip","config":{"gpio":48,"count":1}}
 * Control command (value 原语：rgb 数组作用于全部灯珠)：
 *   {"action":"led1","value":{"rgb":[255,0,0]}}
 * -------------------------------------------------------------------------- */

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
