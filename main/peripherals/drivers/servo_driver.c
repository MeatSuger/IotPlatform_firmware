#include "servo_driver.h"

#include <stdlib.h>
#include <inttypes.h>

#include "common.h"
#include "cJSON.h"
#include "ledc_pool.h"

#if PERIPH_SERVO_ENABLE

#define SERVO_TIMER       LEDC_TIMER_1
#define SERVO_FREQ_HZ     50
#define SERVO_RESOLUTION  LEDC_TIMER_13_BIT
#define SERVO_PERIOD_US   20000   /* 50 Hz */

/* Driver-private data (dev->drvdata). */
typedef struct
{
    int gpio;
    uint32_t min_pulse_us;
    uint32_t max_pulse_us;
    int min_angle;
    int max_angle;
} servo_dev_t;

static bool g_timer_ready = false;

static uint32_t pulse_to_duty(uint32_t pulse_us)
{
    return pulse_us * 8191 / SERVO_PERIOD_US;
}

static bool servo_probe(periph_device_t *dev, const cJSON *cfg)
{
    int gpio = SERVO_GPIO_DEFAULT;
    uint32_t min_pulse_us = 500, max_pulse_us = 2500;
    int min_angle = 0, max_angle = 180;

    if (cfg != NULL)
    {
        cJSON *j = cJSON_GetObjectItem(cfg, "gpio");
        if (cJSON_IsNumber(j))
            gpio = j->valueint;
        j = cJSON_GetObjectItem(cfg, "min_pulse_us");
        if (cJSON_IsNumber(j))
            min_pulse_us = (uint32_t)j->valueint;
        j = cJSON_GetObjectItem(cfg, "max_pulse_us");
        if (cJSON_IsNumber(j))
            max_pulse_us = (uint32_t)j->valueint;
        j = cJSON_GetObjectItem(cfg, "min_angle");
        if (cJSON_IsNumber(j))
            min_angle = j->valueint;
        j = cJSON_GetObjectItem(cfg, "max_angle");
        if (cJSON_IsNumber(j))
            max_angle = j->valueint;
    }

    if (gpio < 0 || max_pulse_us <= min_pulse_us || max_angle <= min_angle)
        return false;

    if (!g_timer_ready)
    {
        ledc_timer_config_t timer_cfg = {
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .duty_resolution = SERVO_RESOLUTION,
            .timer_num = SERVO_TIMER,
            .freq_hz = SERVO_FREQ_HZ,
            .clk_cfg = LEDC_AUTO_CLK,
        };
        if (ledc_timer_config(&timer_cfg) != ESP_OK)
            return false;
        g_timer_ready = true;
    }

    if (ledc_pool_acquire(gpio, SERVO_TIMER) < 0)
        return false;

    servo_dev_t *sd = calloc(1, sizeof *sd);
    if (sd == NULL)
    {
        ledc_pool_release(gpio);
        return false;
    }
    sd->gpio = gpio;
    sd->min_pulse_us = min_pulse_us;
    sd->max_pulse_us = max_pulse_us;
    sd->min_angle = min_angle;
    sd->max_angle = max_angle;
    dev->drvdata = sd;

    /* Neutral position (midpoint of the pulse range). */
    ledc_pool_set_duty(gpio, pulse_to_duty((min_pulse_us + max_pulse_us) / 2));

    DEBUG_PRINTLN("servo[%s] ready on GPIO %d (%" PRIu32 "-%" PRIu32 " us, %d-%d deg)",
                  dev->name, gpio, min_pulse_us, max_pulse_us,
                  min_angle, max_angle);
    return true;
}

static void servo_remove(periph_device_t *dev)
{
    servo_dev_t *sd = (servo_dev_t *)dev->drvdata;
    if (sd == NULL)
        return;

    ledc_pool_release(sd->gpio);
    free(sd);
    dev->drvdata = NULL;
}

static bool servo_command(periph_device_t *dev, const cJSON *pl)
{
    servo_dev_t *sd = (servo_dev_t *)dev->drvdata;
    if (sd == NULL)
        return false;

    cJSON *value = cJSON_GetObjectItem(pl, "value");
    if (value == NULL || !cJSON_IsObject(value))
        return false;

    uint32_t pulse_us;

    cJSON *pulse = cJSON_GetObjectItem(value, "pulse_us");
    if (cJSON_IsNumber(pulse))
    {
        pulse_us = (uint32_t)pulse->valueint;
        if (pulse_us < sd->min_pulse_us)
            pulse_us = sd->min_pulse_us;
        if (pulse_us > sd->max_pulse_us)
            pulse_us = sd->max_pulse_us;
    }
    else
    {
        cJSON *angle = cJSON_GetObjectItem(value, "angle");
        if (!cJSON_IsNumber(angle))
            return false;

        int a = angle->valueint;
        if (a < sd->min_angle)
            a = sd->min_angle;
        if (a > sd->max_angle)
            a = sd->max_angle;

        pulse_us = sd->min_pulse_us +
                   (uint32_t)((uint64_t)(sd->max_pulse_us - sd->min_pulse_us) *
                              (a - sd->min_angle) /
                              (sd->max_angle - sd->min_angle));
    }

    return ledc_pool_set_duty(sd->gpio, pulse_to_duty(pulse_us));
}

static const periph_ops_t servo_ops = {
    .command = servo_command,
};

periph_driver_t servo_driver = {
    .name = "servo",
    .ops = &servo_ops,
    .probe = servo_probe,
    .remove = servo_remove,
};

#endif /* PERIPH_SERVO_ENABLE */
