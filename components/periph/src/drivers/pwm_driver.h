#ifndef _PWM_DRIVER_H_
#define _PWM_DRIVER_H_

#include "periph.h"

/* PWM transport — LEDC 脉宽调制输出（风扇 / 调光 / 舵机等通用）。
 * Device config：{"transport":"pwm","pin":18,"freq_hz":25000}
 *   pin 必填（LEDC 通道自动分配）；freq_hz 默认 1000Hz，duty 分辨率按频率自动推导。
 * Control command（value 原语，角度/颜色等语义由云端换算）：
 *   {"action":"fan1","value":{"duty":80}}（0-100%）／{"pulse_us":1500}（舵机脉宽）
 */

extern periph_driver_t pwm_driver;

#endif /* _PWM_DRIVER_H_ */
