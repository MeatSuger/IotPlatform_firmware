#include "ledc_pool.h"

#include <math.h>

#include "common.h"

/* --------------------------------------------------------------------------
 * LEDC channel/timer pool — generic PWM resource manager.
 *
 * Timers are shared: all channels with the same frequency bind to one
 * timer (LEDC timers are the scarce resource, 4 on ESP32-S3; channels
 * are 8).  Duty resolution is derived from the frequency so that the
 * timer can represent it (res = min(14, log2(clk / freq))).
 * -------------------------------------------------------------------------- */

#define LEDC_MODE        LEDC_LOW_SPEED_MODE
#define LEDC_CLK_HZ      (80U * 1000 * 1000) /* APB clock, S3 */
#define LEDC_POOL_SLOTS  8                   /* LEDC_CHANNEL_MAX */
#define LEDC_POOL_TIMERS 4                   /* LEDC_TIMER_MAX */

typedef struct
{
    bool in_use;
    ledc_timer_t timer;
    uint32_t freq_hz;
    uint32_t resolution_bit;
} ledc_timer_slot_t;

typedef struct
{
    int gpio;
    ledc_channel_t channel;
    ledc_timer_t timer;
    bool in_use;
} ledc_chan_slot_t;

static ledc_timer_slot_t g_timers[LEDC_POOL_TIMERS];
static ledc_chan_slot_t g_channels[LEDC_POOL_SLOTS];
static bool g_inited = false;

/* 由频率推导 duty 分辨率（bit），并 clamp 到 [1, 14] */
static uint32_t resolution_for(uint32_t freq_hz)
{
    if (freq_hz == 0)
        return 0;
    double res = log2((double)LEDC_CLK_HZ / (double)freq_hz);
    uint32_t r = (uint32_t)res;
    if (r < 1)
        r = 1;
    if (r > 14)
        r = 14;
    return r;
}

static void pool_init(void)
{
    if (g_inited)
        return;
    g_inited = true;

    for (int i = 0; i < LEDC_POOL_SLOTS; i++)
    {
        g_channels[i].gpio = -1;
        g_channels[i].channel = (ledc_channel_t)i;
        g_channels[i].timer = LEDC_TIMER_MAX;
        g_channels[i].in_use = false;
    }
    for (int i = 0; i < LEDC_POOL_TIMERS; i++)
    {
        g_timers[i].in_use = false;
        g_timers[i].timer = (ledc_timer_t)i;
        g_timers[i].freq_hz = 0;
        g_timers[i].resolution_bit = 0;
    }
}

/* 找同频已启用 timer；没有则占用一个空闲 timer 并配置 */
static ledc_timer_slot_t *timer_for(uint32_t freq_hz)
{
    uint32_t res = resolution_for(freq_hz);
    if (res == 0)
        return NULL;

    for (int i = 0; i < LEDC_POOL_TIMERS; i++)
    {
        if (g_timers[i].in_use && g_timers[i].freq_hz == freq_hz)
            return &g_timers[i];
    }

    for (int i = 0; i < LEDC_POOL_TIMERS; i++)
    {
        if (!g_timers[i].in_use)
        {
            ledc_timer_config_t tcfg = {
                .speed_mode = LEDC_MODE,
                .duty_resolution = (ledc_timer_bit_t)res,
                .timer_num = g_timers[i].timer,
                .freq_hz = freq_hz,
                .clk_cfg = LEDC_USE_APB_CLK,
            };
            if (ledc_timer_config(&tcfg) != ESP_OK)
            {
                DEBUG_PRINTLN("ledc_pool: timer config failed freq=%uHz",
                              (unsigned)freq_hz);
                return NULL;
            }
            g_timers[i].in_use = true;
            g_timers[i].freq_hz = freq_hz;
            g_timers[i].resolution_bit = res;
            DEBUG_PRINTLN("ledc_pool: timer %d -> %u Hz / %u bit",
                          i, (unsigned)freq_hz, (unsigned)res);
            return &g_timers[i];
        }
    }

    DEBUG_PRINTLN("ledc_pool: no free timer for %u Hz", (unsigned)freq_hz);
    return NULL;
}

int ledc_pool_acquire(int gpio, uint32_t freq_hz)
{
    pool_init();
    if (gpio < 0 || freq_hz == 0)
        return -1;

    ledc_timer_slot_t *tm = timer_for(freq_hz);
    if (tm == NULL)
        return -1;

    /* 引脚已持有：同频直接返回，异频重绑 timer */
    for (int i = 0; i < LEDC_POOL_SLOTS; i++)
    {
        ledc_chan_slot_t *s = &g_channels[i];
        if (s->in_use && s->gpio == gpio)
        {
            if (s->timer == tm->timer)
                return (int)s->channel;

            ledc_channel_config_t ch = {
                .gpio_num = gpio,
                .speed_mode = LEDC_MODE,
                .channel = s->channel,
                .timer_sel = tm->timer,
                .duty = 0,
                .hpoint = 0,
            };
            if (ledc_channel_config(&ch) != ESP_OK)
                return -1;
            s->timer = tm->timer;
            return (int)s->channel;
        }
    }

    for (int i = 0; i < LEDC_POOL_SLOTS; i++)
    {
        ledc_chan_slot_t *s = &g_channels[i];
        if (!s->in_use)
        {
            ledc_channel_config_t ch = {
                .gpio_num = gpio,
                .speed_mode = LEDC_MODE,
                .channel = s->channel,
                .timer_sel = tm->timer,
                .duty = 0,
                .hpoint = 0,
            };
            if (ledc_channel_config(&ch) != ESP_OK)
                return -1;

            s->gpio = gpio;
            s->timer = tm->timer;
            s->in_use = true;
            return (int)s->channel;
        }
    }

    DEBUG_PRINTLN("ledc_pool: all channels in use");
    return -1;
}

bool ledc_pool_release(int gpio)
{
    for (int i = 0; i < LEDC_POOL_SLOTS; i++)
    {
        ledc_chan_slot_t *s = &g_channels[i];
        if (s->in_use && s->gpio == gpio)
        {
            s->in_use = false;
            s->gpio = -1;
            s->timer = LEDC_TIMER_MAX;
            return true;
        }
    }
    return false;
}

static ledc_chan_slot_t *chan_find(int gpio)
{
    for (int i = 0; i < LEDC_POOL_SLOTS; i++)
    {
        if (g_channels[i].in_use && g_channels[i].gpio == gpio)
            return &g_channels[i];
    }
    return NULL;
}

static uint32_t chan_max_ticks(const ledc_chan_slot_t *s)
{
    for (int i = 0; i < LEDC_POOL_TIMERS; i++)
    {
        if (g_timers[i].in_use && g_timers[i].timer == s->timer)
            return (1U << g_timers[i].resolution_bit) - 1U;
    }
    return 0;
}

uint32_t ledc_pool_max_ticks(int gpio)
{
    ledc_chan_slot_t *s = chan_find(gpio);
    if (s == NULL)
        return 0;
    return chan_max_ticks(s);
}

static bool set_ticks(int gpio, uint32_t ticks)
{
    ledc_chan_slot_t *s = chan_find(gpio);
    if (s == NULL)
        return false;

    uint32_t max = chan_max_ticks(s);
    if (ticks > max)
        ticks = max;

    if (ledc_set_duty(LEDC_MODE, s->channel, ticks) != ESP_OK)
        return false;
    return ledc_update_duty(LEDC_MODE, s->channel) == ESP_OK;
}

bool ledc_pool_set_duty_pct(int gpio, int pct)
{
    if (pct < 0)
        pct = 0;
    if (pct > 100)
        pct = 100;

    uint32_t max = ledc_pool_max_ticks(gpio);
    uint32_t ticks = (uint32_t)pct * max / 100U;
    return set_ticks(gpio, ticks);
}

bool ledc_pool_set_pulse_us(int gpio, uint32_t pulse_us)
{
    ledc_chan_slot_t *s = chan_find(gpio);
    if (s == NULL)
        return false;

    uint32_t max = chan_max_ticks(s);
    if (max == 0)
        return false;

    /* ticks = pulse_us/1e6 * freq * max */
    uint64_t freq = 0;
    for (int i = 0; i < LEDC_POOL_TIMERS; i++)
    {
        if (g_timers[i].in_use && g_timers[i].timer == s->timer)
        {
            freq = g_timers[i].freq_hz;
            break;
        }
    }
    if (freq == 0)
        return false;

    uint64_t ticks = (uint64_t)pulse_us * freq * max / 1000000ULL;
    return set_ticks(gpio, (uint32_t)ticks);
}
