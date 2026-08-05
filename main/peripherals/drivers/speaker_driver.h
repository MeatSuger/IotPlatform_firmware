#ifndef _SPEAKER_DRIVER_H_
#define _SPEAKER_DRIVER_H_

#include "periph.h"

/* --------------------------------------------------------------------------
 * Speaker / buzzer driver.
 *
 *   PERIPH_SPEAKER_ENABLE — compile switch (default 1).
 *   SPEAKER_GPIO_DEFAULT  — default pin when config omits "gpio".
 *
 * Device config:
 *   {"driver":"speaker","config":{"gpio":2,"type":"active"}}
 *   type: "active"  — self-oscillating buzzer, driven by GPIO level
 *         "passive" — piezo element, driven by LEDC square wave
 *                     (timer 2, frequency re-tunable)
 * Control commands (action = device name):
 *   {"action":"speaker","value":{"state":"on"}}        — on / off
 *   {"action":"speaker","value":{"state":1}}
 *   {"action":"speaker","value":{"freq":1000}}         — passive: tone
 *   {"action":"speaker","value":{"freq":1000,"duration_ms":200}}
 *       — tone for a fixed duration. Note: blocks the calling task
 *         (cmdProcessTask) for duration_ms; use 0/omit for continuous.
 * -------------------------------------------------------------------------- */

#ifndef PERIPH_SPEAKER_ENABLE
#define PERIPH_SPEAKER_ENABLE 1
#endif

#ifndef SPEAKER_GPIO_DEFAULT
#define SPEAKER_GPIO_DEFAULT 2
#endif

extern periph_driver_t speaker_driver;

#endif /* _SPEAKER_DRIVER_H_ */
