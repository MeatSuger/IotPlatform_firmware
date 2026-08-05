#include "ledc_pool.h"

#include "common.h"

/* --------------------------------------------------------------------------
 * Timer 0: 5000 Hz / 13 bit — legacy analogWrite (PWM) replacement.
 * Other timers are configured by their owning driver (servo, speaker).
 * -------------------------------------------------------------------------- */
#define LEDC_MODE       LEDC_LOW_SPEED_MODE
#define LEDC_RESOLUTION LEDC_TIMER_13_BIT
#define LEDC_FREQ_HZ    5000

#define LEDC_POOL_SLOTS 8

typedef struct
{
    int gpio;
    ledc_channel_t channel;
    ledc_timer_t timer;
    bool in_use;
} ledc_slot_t;

static ledc_slot_t g_slots[LEDC_POOL_SLOTS];
static bool g_initialized = false;

static void pool_init(void)
{
    if (g_initialized)
        return;

    for (int i = 0; i < LEDC_POOL_SLOTS; i++)
    {
        g_slots[i].gpio = -1;
        g_slots[i].channel = (ledc_channel_t)i;
        g_slots[i].timer = LEDC_TIMER_MAX;
        g_slots[i].in_use = false;
    }

    ledc_timer_config_t timer_cfg = {
        .speed_mode = LEDC_MODE,
        .duty_resolution = LEDC_RESOLUTION,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer_cfg));
    g_initialized = true;
}

static ledc_slot_t *slot_find(int gpio)
{
    for (int i = 0; i < LEDC_POOL_SLOTS; i++)
    {
        if (g_slots[i].in_use && g_slots[i].gpio == gpio)
            return &g_slots[i];
    }
    return NULL;
}

int ledc_pool_acquire(int gpio, ledc_timer_t timer)
{
    pool_init();

    /* Already held: rebind the timer if the owner changed it. */
    ledc_slot_t *slot = slot_find(gpio);
    if (slot != NULL)
    {
        if (slot->timer == timer)
            return (int)slot->channel;

        ledc_channel_config_t ch_cfg = {
            .gpio_num = gpio,
            .speed_mode = LEDC_MODE,
            .channel = slot->channel,
            .timer_sel = timer,
            .duty = 0,
            .hpoint = 0,
        };
        if (ledc_channel_config(&ch_cfg) != ESP_OK)
            return -1;
        slot->timer = timer;
        return (int)slot->channel;
    }

    for (int i = 0; i < LEDC_POOL_SLOTS; i++)
    {
        if (!g_slots[i].in_use)
        {
            g_slots[i].gpio = gpio;
            g_slots[i].timer = timer;
            g_slots[i].in_use = true;

            ledc_channel_config_t ch_cfg = {
                .gpio_num = gpio,
                .speed_mode = LEDC_MODE,
                .channel = g_slots[i].channel,
                .timer_sel = timer,
                .duty = 0,
                .hpoint = 0,
            };
            if (ledc_channel_config(&ch_cfg) != ESP_OK)
            {
                g_slots[i].in_use = false;
                g_slots[i].gpio = -1;
                return -1;
            }
            return (int)g_slots[i].channel;
        }
    }

    DEBUG_PRINTLN("WARNING: all LEDC channels in use");
    return -1;
}

bool ledc_pool_release(int gpio)
{
    ledc_slot_t *slot = slot_find(gpio);
    if (slot == NULL)
        return false;

    slot->in_use = false;
    slot->gpio = -1;
    slot->timer = LEDC_TIMER_MAX;
    return true;
}

bool ledc_pool_set_duty(int gpio, uint32_t duty)
{
    ledc_slot_t *slot = slot_find(gpio);
    if (slot == NULL)
        return false;

    if (ledc_set_duty(LEDC_MODE, slot->channel, duty) != ESP_OK)
        return false;
    return ledc_update_duty(LEDC_MODE, slot->channel) == ESP_OK;
}

bool ledc_pool_set_duty_pct(int gpio, int duty_0_255)
{
    if (duty_0_255 < 0)
        duty_0_255 = 0;
    if (duty_0_255 > 255)
        duty_0_255 = 255;

    int ch = ledc_pool_acquire(gpio, LEDC_TIMER_0);
    if (ch < 0)
        return false;

    uint32_t scaled = (uint32_t)duty_0_255 * 8191 / 255;
    return ledc_pool_set_duty(gpio, scaled);
}
