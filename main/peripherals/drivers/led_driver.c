#include "led_driver.h"

#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "cJSON.h"
#include "led_strip.h"

#if PERIPH_LED_ENABLE

/* Driver-private data (dev->drvdata). */
typedef struct
{
    led_strip_handle_t strip;
    int count;
} led_dev_t;

static bool led_probe(periph_device_t *dev, const cJSON *cfg)
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
        return false;

    led_dev_t *ld = calloc(1, sizeof *ld);
    if (ld == NULL)
    {
        led_strip_del(strip);
        return false;
    }
    ld->strip = strip;
    ld->count = count;
    dev->drvdata = ld;

    led_strip_clear(strip);
    DEBUG_PRINTLN("led[%s] ready: %d LED(s) on GPIO %d",
                  dev->name, count, gpio);
    return true;
}

static void led_remove(periph_device_t *dev)
{
    led_dev_t *ld = (led_dev_t *)dev->drvdata;
    if (ld == NULL)
        return;

    if (ld->strip != NULL)
        led_strip_del(ld->strip);
    free(ld);
    dev->drvdata = NULL;
}

static bool led_command(periph_device_t *dev, const cJSON *pl)
{
    led_dev_t *ld = (led_dev_t *)dev->drvdata;
    if (ld == NULL || ld->strip == NULL)
        return false;

    cJSON *value = cJSON_GetObjectItem(pl, "value");
    if (value == NULL || !cJSON_IsObject(value))
        return false;

    cJSON *r = cJSON_GetObjectItem(value, "r");
    cJSON *g = cJSON_GetObjectItem(value, "g");
    cJSON *b = cJSON_GetObjectItem(value, "b");
    if (!cJSON_IsNumber(r) || !cJSON_IsNumber(g) || !cJSON_IsNumber(b))
        return false;

    uint32_t red = (uint32_t)(r->valueint < 0 ? 0 :
                              (r->valueint > 255 ? 255 : r->valueint));
    uint32_t green = (uint32_t)(g->valueint < 0 ? 0 :
                                (g->valueint > 255 ? 255 : g->valueint));
    uint32_t blue = (uint32_t)(b->valueint < 0 ? 0 :
                               (b->valueint > 255 ? 255 : b->valueint));

    /* Single RGB LED on this board: set every LED in the strip. */
    for (int i = 0; i < ld->count; i++)
    {
        if (led_strip_set_pixel(ld->strip, (uint32_t)i,
                                red, green, blue) != ESP_OK)
            return false;
    }

    return led_strip_refresh(ld->strip) == ESP_OK;
}

static const periph_ops_t led_ops = {
    .command = led_command,
};

periph_driver_t led_driver = {
    .name = "led",
    .ops = &led_ops,
    .probe = led_probe,
    .remove = led_remove,
};

#endif /* PERIPH_LED_ENABLE */
