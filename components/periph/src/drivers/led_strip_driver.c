#include "led_strip_driver.h"

#include <stdlib.h>

#include "common.h"
#include "cJSON.h"
#include "led_strip.h"

#if PERIPH_LED_ENABLE

/* Driver-private data (dev->drvdata). */
typedef struct
{
    led_strip_handle_t strip;
    int gpio;
    int count;
} led_strip_dev_t;

static bool led_strip_probe(periph_device_t *dev, const cJSON *cfg)
{
    int gpio = WS2812_GPIO;
    int count = WS2812_LED_COUNT;

    if (cfg != NULL)
    {
        cJSON *j = cJSON_GetObjectItem(cfg, "gpio");
        if (cJSON_IsNumber(j))
            gpio = j->valueint;
        j = cJSON_GetObjectItem(cfg, "count");
        if (cJSON_IsNumber(j))
            count = j->valueint;
    }

    if (gpio < 0 || count < 1 || count > 512)
        return false;
    if (!periph_pin_claim(gpio, dev->name))
        return false;

    led_strip_config_t strip_cfg = {
        .strip_gpio_num = gpio,
        .max_leds = (uint32_t)count,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000, /* 10 MHz tick */
        .mem_block_symbols = 0,            /* driver default size */
        .flags.with_dma = false,
    };

    led_strip_handle_t strip = NULL;
    if (led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &strip) != ESP_OK)
    {
        periph_pin_release(gpio);
        return false;
    }

    led_strip_dev_t *ld = calloc(1, sizeof *ld);
    if (ld == NULL)
    {
        led_strip_del(strip);
        periph_pin_release(gpio);
        return false;
    }
    ld->strip = strip;
    ld->gpio = gpio;
    ld->count = count;
    dev->drvdata = ld;

    led_strip_clear(strip);
    DEBUG_PRINTLN("led_strip[%s] ready: %d LED(s) on GPIO %d",
                  dev->name, count, gpio);
    return true;
}

static void led_strip_remove(periph_device_t *dev)
{
    led_strip_dev_t *ld = (led_strip_dev_t *)dev->drvdata;
    if (ld == NULL)
        return;

    if (ld->strip != NULL)
        led_strip_del(ld->strip);
    periph_pin_release(ld->gpio);
    free(ld);
    dev->drvdata = NULL;
}

static bool led_strip_command(periph_device_t *dev, const cJSON *pl)
{
    led_strip_dev_t *ld = (led_strip_dev_t *)dev->drvdata;
    if (ld == NULL || ld->strip == NULL)
        return false;

    cJSON *value = cJSON_GetObjectItem(pl, "value");
    if (value == NULL || !cJSON_IsObject(value))
        return false;

    /* value: {"rgb":[r,g,b]} —— rgb 原语，云端负责颜色语义 */
    cJSON *rgb = cJSON_GetObjectItem(value, "rgb");
    if (!cJSON_IsArray(rgb) || cJSON_GetArraySize(rgb) != 3)
    {
        DEBUG_PRINTLN("led_strip[%s]: value.rgb 需为 [r,g,b]",
                      dev->name);
        return false;
    }

    int comp[3];
    for (int i = 0; i < 3; i++)
    {
        cJSON *e = cJSON_GetArrayItem(rgb, i);
        if (!cJSON_IsNumber(e))
            return false;
        int v = e->valueint;
        comp[i] = v < 0 ? 0 : (v > 255 ? 255 : v);
    }

    for (int i = 0; i < ld->count; i++)
    {
        if (led_strip_set_pixel(ld->strip, (uint32_t)i,
                                (uint32_t)comp[0], (uint32_t)comp[1],
                                (uint32_t)comp[2]) != ESP_OK)
            return false;
    }

    return led_strip_refresh(ld->strip) == ESP_OK;
}

static const periph_ops_t led_strip_ops = {
    .command = led_strip_command,
};

periph_driver_t led_strip_driver = {
    .name = "led_strip",
    .ops = &led_strip_ops,
    .probe = led_strip_probe,
    .remove = led_strip_remove,
};

#endif /* PERIPH_LED_ENABLE */
