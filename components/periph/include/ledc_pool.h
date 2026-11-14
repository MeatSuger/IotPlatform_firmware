#ifndef _LEDC_POOL_H_
#define _LEDC_POOL_H_

#include <stdbool.h>
#include <stdint.h>

#include "driver/ledc.h" /* LEDC_LOW_SPEED_MODE */

/* LEDC channel/timer pool — PWM 资源管理：按 GPIO + 目标频率取通道，优先复用
 * 同频已启用的 timer，没有才配置空闲 timer（S3 只有 4 个 timer，通道 8 个）；
 * duty 分辨率由频率自动推导。所有 duty setter 立即生效。
 */

/* timer 时钟源显式选 XTAL 40MHz：APB 不能跨 light sleep 保持，DFS 降频也会
 * 破坏波形；配 sleep_mode 后舵机/风扇可跨睡眠持续驱动 */
#define LEDC_POOL_MODE LEDC_LOW_SPEED_MODE
#define LEDC_POOL_CLK_HZ (40U * 1000 * 1000)
#define LEDC_POOL_SLOTS 8  /* LEDC_CHANNEL_MAX */
#define LEDC_POOL_TIMERS 4 /* LEDC_TIMER_MAX */

/* 为 GPIO 取指定频率（Hz）的通道；失败（引脚已占/无空闲 timer）返回 -1 */
int ledc_pool_acquire(int gpio, uint32_t freq_hz);

/* 释放该 GPIO 持有的通道 */
bool ledc_pool_release(int gpio);

/* 该引脚 timer 的满量程 tick（2^res - 1） */
uint32_t ledc_pool_max_ticks(int gpio);

/* 设置占空比整数百分比 [0, 100] */
bool ledc_pool_set_duty_pct(int gpio, int pct);

/* 按脉宽微秒设置占空比（0 = 全关） */
bool ledc_pool_set_pulse_us(int gpio, uint32_t pulse_us);

#endif /* _LEDC_POOL_H_ */
