#include "pwm_driver.h"

#include <stdlib.h>
#include <stdint.h>

#include "common.h"
#include "esp_log.h"
#include "cJSON.h"
#include "ledc_pool.h"

static const char *TAG = "pwm";

/* Driver-private data (dev->drvdata). */
typedef struct
{
    int pin;
} pwm_dev_t;

static bool pwm_probe(periph_device_t *dev, const cJSON *cfg)
{
    cJSON *pin = cJSON_GetObjectItem(cfg, "pin");
    if (!cJSON_IsNumber(pin))
    {
        ESP_LOGE(TAG, "pwm[%s]: config.pin 缺失", dev->name);
        return false;
    }
    int gpio = pin->valueint;
    if (gpio < 0 || !periph_pin_claim(gpio, dev->name))
        return false;

    uint32_t freq = 1000;
    cJSON *f = cJSON_GetObjectItem(cfg, "freq_hz");
    if (cJSON_IsNumber(f) && f->valueint > 0)
        freq = (uint32_t)f->valueint;

    if (ledc_pool_acquire(gpio, freq) < 0)
    {
        periph_pin_release(gpio);
        ESP_LOGE(TAG, "pwm[%s]: LEDC 通道分配失败 (pin=%d freq=%uHz)",
                      dev->name, gpio, (unsigned)freq);
        return false;
    }

    pwm_dev_t *p = calloc(1, sizeof *p);
    if (p == NULL)
    {
        ledc_pool_release(gpio);
        periph_pin_release(gpio);
        return false;
    }
    p->pin = gpio;
    dev->drvdata = p;

    ledc_pool_set_duty_pct(gpio, 0);
    ESP_LOGI(TAG, "pwm[%s] ready: pin=%d freq=%uHz", dev->name, gpio,
                  (unsigned)freq);
    return true;
}

static void pwm_remove(periph_device_t *dev)
{
    pwm_dev_t *p = (pwm_dev_t *)dev->drvdata;
    if (p == NULL)
        return;

    ledc_pool_set_duty_pct(p->pin, 0);
    ledc_pool_release(p->pin);
    periph_pin_release(p->pin);
    free(p);
    dev->drvdata = NULL;
}

static bool pwm_command(periph_device_t *dev, const cJSON *pl)
{
    pwm_dev_t *p = (pwm_dev_t *)dev->drvdata;
    if (p == NULL)
        return false;

    cJSON *value = cJSON_GetObjectItem(pl, "value");
    if (value == NULL || !cJSON_IsObject(value))
        return false;

    /* pulse_us 优先（舵机类脉宽直给）；否则 duty 百分比 */
    cJSON *pulse = cJSON_GetObjectItem(value, "pulse_us");
    if (cJSON_IsNumber(pulse) && pulse->valueint >= 0)
        return ledc_pool_set_pulse_us(p->pin, (uint32_t)pulse->valueint);

    cJSON *duty = cJSON_GetObjectItem(value, "duty");
    if (cJSON_IsNumber(duty))
        return ledc_pool_set_duty_pct(p->pin, duty->valueint);

    ESP_LOGW(TAG, "pwm[%s]: value 需含 duty(0-100) 或 pulse_us", dev->name);
    return false;
}

static const periph_ops_t pwm_ops = {
    .command = pwm_command,
};

periph_driver_t pwm_driver = {
    .name = "pwm",
    .ops = &pwm_ops,
    .probe = pwm_probe,
    .remove = pwm_remove,
};
