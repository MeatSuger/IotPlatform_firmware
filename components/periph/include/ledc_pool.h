#ifndef _LEDC_POOL_H_
#define _LEDC_POOL_H_

#include <stdbool.h>
#include <stdint.h>

/* --------------------------------------------------------------------------
 * LEDC channel/timer pool — generic PWM resource manager.
 *
 * PWM devices (transport "pwm") acquire a channel per GPIO pin with a
 * desired frequency; the pool picks a timer already running at that
 * frequency, or configures a free one (up to 4 timers, shared by all
 * channels of the same frequency).  Duty resolution is derived from the
 * frequency automatically.
 *
 * All duty setters take effect immediately (update duty is called).
 * -------------------------------------------------------------------------- */

/* Acquire a PWM channel for a GPIO pin at the given frequency (Hz).
 * Returns the channel index, or -1 on failure (pin busy / no timer). */
int ledc_pool_acquire(int gpio, uint32_t freq_hz);

/* Release the channel held by a GPIO pin. */
bool ledc_pool_release(int gpio);

/* Full-scale duty count for the pin's timer resolution (2^res - 1). */
uint32_t ledc_pool_max_ticks(int gpio);

/* Set duty as a percentage in [0, 100] (integer percent). */
bool ledc_pool_set_duty_pct(int gpio, int pct);

/* Set duty from a pulse width in microseconds (0 = fully off). */
bool ledc_pool_set_pulse_us(int gpio, uint32_t pulse_us);

#endif /* _LEDC_POOL_H_ */
