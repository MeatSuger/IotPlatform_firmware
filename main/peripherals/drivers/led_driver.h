#ifndef _LED_DRIVER_H_
#define _LED_DRIVER_H_

#include "periph.h"

/* --------------------------------------------------------------------------
 * LED (WS2812) driver — espressif/led_strip over RMT.
 *
 * Compatibility macros:
 *   WS2812_ENABLE     — master switch for the LED driver (default 1).
 *                       Kept from the original led.c module.
 *   PERIPH_LED_ENABLE — alias, defaults to WS2812_ENABLE. Set to 0 to
 *                       compile the driver out entirely.
 *   WS2812_GPIO       — default data pin when config omits "gpio".
 *   WS2812_LED_COUNT  — default LED count when config omits "count".
 *
 * Device config (MQTT "config" command):
 *   {"driver":"led","config":{"gpio":48,"count":1}}
 * Control command (action = device name):
 *   {"action":"led","value":{"r":255,"g":0,"b":0}}              — all LEDs
 *   {"action":"led","value":{"r":0,"g":255,"b":0,"index":1}}    — one LED
 * -------------------------------------------------------------------------- */

#ifndef WS2812_ENABLE
#define WS2812_ENABLE 1
#endif

#ifndef PERIPH_LED_ENABLE
#define PERIPH_LED_ENABLE WS2812_ENABLE
#endif

#ifndef WS2812_GPIO
#define WS2812_GPIO 48   /* ESP32-S3-DevKitC-1 on-board WS2812 */
#endif

#ifndef WS2812_LED_COUNT
#define WS2812_LED_COUNT 1
#endif

extern periph_driver_t led_driver;

#endif /* _LED_DRIVER_H_ */
