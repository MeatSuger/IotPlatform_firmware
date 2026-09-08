#include "gpio_driver.h"

#include <stdlib.h>

#include "common.h"
#include "esp_log.h"
#include "cJSON.h"
#include "driver/gpio.h"

static const char *TAG = "gpio";

/* Driver-private data (dev->drvdata). */
typedef struct
{
    int pin;
    bool active_high;
} gpio_dev_t;

/* 逻辑电平 → 物理电平（active_high 配置） */
static int to_physical(const gpio_dev_t *g, int level)
{
    return g->active_high ? level : (level ? 0 : 1);
}

static bool gpio_probe(periph_device_t *dev, const cJSON *cfg)
{
    cJSON *pin = cJSON_GetObjectItem(cfg, "pin");
    if (!cJSON_IsNumber(pin))
    {
        ESP_LOGE(TAG, "gpio[%s]: config.pin 缺失", dev->name);
        return false;
    }
    int gpio = pin->valueint;

    /* 引脚仲裁：同引脚不得被多个设备占用 */
    if (gpio < 0)
        return false;
    if (!periph_pin_claim(gpio, dev->name))
        return false;

    bool active_high = true;
    cJSON *ah = cJSON_GetObjectItem(cfg, "active_high");
    if (cJSON_IsBool(ah))
        active_high = cJSON_IsTrue(ah);

    int initial = 0;
    cJSON *init = cJSON_GetObjectItem(cfg, "initial");
    if (cJSON_IsNumber(init))
        initial = init->valueint ? 1 : 0;

    gpio_dev_t *g = calloc(1, sizeof *g);
    if (g == NULL)
    {
        periph_pin_release(gpio);
        return false;
    }
    g->pin = gpio;
    g->active_high = active_high;
    dev->drvdata = g;

    gpio_reset_pin(gpio);
    gpio_set_direction(gpio, GPIO_MODE_OUTPUT);
    gpio_set_level(gpio, to_physical(g, initial));
    ESP_LOGI(TAG, "gpio[%s] ready: pin=%d active_high=%d initial=%d",
                  dev->name, gpio, active_high, initial);
    return true;
}

static void gpio_remove(periph_device_t *dev)
{
    gpio_dev_t *g = (gpio_dev_t *)dev->drvdata;
    if (g == NULL)
        return;

    periph_pin_release(g->pin);
    free(g);
    dev->drvdata = NULL;
}

static bool gpio_command(periph_device_t *dev, const cJSON *pl)
{
    gpio_dev_t *g = (gpio_dev_t *)dev->drvdata;
    if (g == NULL)
        return false;

    cJSON *value = cJSON_GetObjectItem(pl, "value");
    if (value == NULL || !cJSON_IsObject(value))
        return false;

    cJSON *toggle = cJSON_GetObjectItem(value, "toggle");
    if (cJSON_IsTrue(toggle))
    {
        int cur = gpio_get_level(g->pin);
        gpio_set_level(g->pin, cur ? 0 : 1);
        return true;
    }

    cJSON *level = cJSON_GetObjectItem(value, "level");
    if (cJSON_IsNumber(level))
    {
        int lv = level->valueint ? 1 : 0;
        gpio_set_level(g->pin, to_physical(g, lv));
        return true;
    }

    ESP_LOGW(TAG, "gpio[%s]: value 需含 level 或 toggle", dev->name);
    return false;
}

static const periph_ops_t gpio_ops = {
    .command = gpio_command,
};

periph_driver_t gpio_driver = {
    .name = "gpio",
    .ops = &gpio_ops,
    .probe = gpio_probe,
    .remove = gpio_remove,
};
