#ifndef _LEDC_POOL_H_
#define _LEDC_POOL_H_

#include <stdbool.h>
#include <stdint.h>
#include "driver/ledc.h"

/* --------------------------------------------------------------------------
 * LEDC channel pool — shared by all PWM users (GPIO "pwm" action, servo,
 * passive speaker). Channels are acquired per GPIO pin, so every user can
 * grab a channel without stepping on each other.
 *
 * Timers are owned by the users:
 *   LEDC_TIMER_0 — 5000 Hz / 13 bit (legacy analogWrite semantics)
 *   LEDC_TIMER_1 — 50 Hz / 13 bit (servo)
 *   LEDC_TIMER_2 — 1 kHz / 13 bit (passive speaker, freq is re-tunable)
 *
 * Returns the acquired channel, or -1 on failure. */
int ledc_pool_acquire(int gpio, ledc_timer_t timer);

/* Release the channel held by a GPIO pin. */
bool ledc_pool_release(int gpio);

/* Set raw duty (in timer resolution ticks) for a GPIO pin. */
bool ledc_pool_set_duty(int gpio, uint32_t duty);

/* Arduino-style analogWrite: duty in [0, 255], scaled to the 13-bit
 * timer resolution (timer 0, 5000 Hz). Acquires the channel implicitly. */
bool ledc_pool_set_duty_pct(int gpio, int duty_0_255);

#endif /* _LEDC_POOL_H_ */
