#ifndef _SERVO_DRIVER_H_
#define _SERVO_DRIVER_H_

#include "periph.h"

/* --------------------------------------------------------------------------
 * Servo driver — LEDC PWM at 50 Hz (timer 1, 13-bit).
 *
 *   PERIPH_SERVO_ENABLE — compile switch (default 1).
 *   SERVO_GPIO_DEFAULT  — default signal pin when config omits "gpio".
 *
 * Device config:
 *   {"driver":"servo",
 *    "config":{"gpio":18,"min_pulse_us":500,"max_pulse_us":2500,
 *              "min_angle":0,"max_angle":180}}
 * Control command (action = device name):
 *   {"action":"servo","value":{"angle":90}}      — angle mapped to pulse
 *   {"action":"servo","value":{"pulse_us":1500}} — raw pulse width
 * -------------------------------------------------------------------------- */

#ifndef PERIPH_SERVO_ENABLE
#define PERIPH_SERVO_ENABLE 1
#endif

#ifndef SERVO_GPIO_DEFAULT
#define SERVO_GPIO_DEFAULT 18
#endif

extern periph_driver_t servo_driver;

#endif /* _SERVO_DRIVER_H_ */
