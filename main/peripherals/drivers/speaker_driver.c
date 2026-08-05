#include "speaker_driver.h"

#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "cJSON.h"
#include "driver/gpio.h"
#include "ledc_pool.h"
#include "freertos/task.h"

#if PERIPH_SPEAKER_ENABLE

#define SPEAKER_TIMER       LEDC_TIMER_2
#define SPEAKER_FREQ_DEF   1000   /* Hz */
#define SPEAKER_RESOLUTION LEDC_TIMER_13_BIT
#define SPEAKER_DUTY_ON    (8191 / 2)   /* 50 % duty square wave */

/* Driver-private data (dev->drvdata). */
typedef struct
{
    int gpio;
    bool passive;
} speaker_dev_t;

static bool g_timer_ready = false;

static bool speaker_tone(speaker_dev_t *sd, uint32_t freq_hz, bool on)
{
    if (freq_hz < 100)
        freq_hz = 100;
    if (freq_hz > 20000)
        freq_hz = 20000;

    if (ledc_set_freq(LEDC_LOW_SPEED_MODE, SPEAKER_TIMER, freq_hz) != ESP_OK)
        return false;
    return ledc_pool_set_duty(sd->gpio, on ? SPEAKER_DUTY_ON : 0);
}

static bool speaker_probe(periph_device_t *dev, const cJSON *cfg)
{
    int gpio = SPEAKER_GPIO_DEFAULT;
    bool passive = false;

    if (cfg != NULL)
    {
        cJSON *j = cJSON_GetObjectItem(cfg, "gpio");
        if (cJSON_IsNumber(j))
            gpio = j->valueint;
        j = cJSON_GetObjectItem(cfg, "type");
        if (cJSON_IsString(j) && strcmp(j->valuestring, "passive") == 0)
            passive = true;
    }

    if (gpio < 0)
        return false;

    speaker_dev_t *sd = calloc(1, sizeof *sd);
    if (sd == NULL)
        return false;
    sd->gpio = gpio;
    sd->passive = passive;

    if (passive)
    {
        if (!g_timer_ready)
        {
            ledc_timer_config_t timer_cfg = {
                .speed_mode = LEDC_LOW_SPEED_MODE,
                .duty_resolution = SPEAKER_RESOLUTION,
                .timer_num = SPEAKER_TIMER,
                .freq_hz = SPEAKER_FREQ_DEF,
                .clk_cfg = LEDC_AUTO_CLK,
            };
            if (ledc_timer_config(&timer_cfg) != ESP_OK)
            {
                free(sd);
                return false;
            }
            g_timer_ready = true;
        }
        if (ledc_pool_acquire(gpio, SPEAKER_TIMER) < 0)
        {
            free(sd);
            return false;
        }
        ledc_pool_set_duty(gpio, 0);   /* start silent */
    }
    else
    {
        gpio_reset_pin(gpio);
        gpio_set_direction(gpio, GPIO_MODE_OUTPUT);
        gpio_set_level(gpio, 0);       /* start silent */
    }

    dev->drvdata = sd;
    DEBUG_PRINTLN("speaker[%s] ready on GPIO %d (%s)", dev->name, gpio,
                  passive ? "passive" : "active");
    return true;
}

static void speaker_remove(periph_device_t *dev)
{
    speaker_dev_t *sd = (speaker_dev_t *)dev->drvdata;
    if (sd == NULL)
        return;

    if (sd->passive)
    {
        ledc_pool_set_duty(sd->gpio, 0);
        ledc_pool_release(sd->gpio);
    }
    else
    {
        gpio_set_level(sd->gpio, 0);
        gpio_reset_pin(sd->gpio);
    }

    free(sd);
    dev->drvdata = NULL;
}

static bool speaker_command(periph_device_t *dev, const cJSON *pl)
{
    speaker_dev_t *sd = (speaker_dev_t *)dev->drvdata;
    if (sd == NULL)
        return false;

    cJSON *value = cJSON_GetObjectItem(pl, "value");
    if (value == NULL || !cJSON_IsObject(value))
        return false;

    cJSON *freq = cJSON_GetObjectItem(value, "freq");

    if (cJSON_IsNumber(freq))
    {
        /* Passive-only tone command. */
        if (!sd->passive)
            return false;

        uint32_t freq_hz = (uint32_t)freq->valueint;
        if (!speaker_tone(sd, freq_hz, true))
            return false;

        cJSON *dur = cJSON_GetObjectItem(value, "duration_ms");
        if (cJSON_IsNumber(dur) && dur->valueint > 0)
        {
            vTaskDelay(pdMS_TO_TICKS(dur->valueint));
            speaker_tone(sd, freq_hz, false);
        }
        return true;
    }

    /* state: on / off (string or number) */
    bool on;
    cJSON *state = cJSON_GetObjectItem(value, "state");
    if (cJSON_IsString(state))
    {
        if (strcmp(state->valuestring, "on") == 0)
            on = true;
        else if (strcmp(state->valuestring, "off") == 0)
            on = false;
        else
            return false;
    }
    else if (cJSON_IsNumber(state))
    {
        on = state->valueint != 0;
    }
    else
    {
        return false;
    }

    if (sd->passive)
        return speaker_tone(sd, SPEAKER_FREQ_DEF, on);
    else
    {
        gpio_set_level(sd->gpio, on ? 1 : 0);
        return true;
    }
}

static const periph_ops_t speaker_ops = {
    .command = speaker_command,
};

periph_driver_t speaker_driver = {
    .name = "speaker",
    .ops = &speaker_ops,
    .probe = speaker_probe,
    .remove = speaker_remove,
};

#endif /* PERIPH_SPEAKER_ENABLE */
