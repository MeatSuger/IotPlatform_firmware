// epd transport 胶水：引脚仲裁 + 命令转发，驱动在 display 组件。
// config: {"transport":"epd"}；control: {"value":{"op":"selftest"}}。

#include "drivers/display_driver.h"

#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "display.h"
#include "esp_log.h"

static const char *TAG = "display";

static const int s_pins[] = {DISPLAY_PIN_MOSI, DISPLAY_PIN_SCK,
                             DISPLAY_PIN_CS,   DISPLAY_PIN_DC,
                             DISPLAY_PIN_RST,  DISPLAY_PIN_BUSY};
static bool s_pins_claimed;

static bool display_claim_pins(const char *owner) {
  if (s_pins_claimed) {
    return true;
  }
  for (size_t i = 0; i < sizeof(s_pins) / sizeof(s_pins[0]); i++) {
    if (!periph_pin_claim(s_pins[i], owner)) {
      for (size_t j = 0; j < i; j++) {
        periph_pin_release(s_pins[j]);
      }
      return false;
    }
  }
  s_pins_claimed = true;
  return true;
}

/* 可选像素字段：未配置 present=false；配置了但非法返回 false */
static bool display_opt_pixels(const cJSON *cfg, const char *key,
                               bool *present, unsigned *value) {
  const cJSON *item = cJSON_GetObjectItem(cfg, key);
  *present = (item != NULL);
  if (item == NULL) {
    return true;
  }
  if (!cJSON_IsNumber(item) || item->valuedouble < 1 ||
      item->valuedouble > 4096 ||
      item->valuedouble != (double)(unsigned)item->valuedouble) {
    ESP_LOGE(TAG, "display: specs.%s 非法(需 1-4096 的整数)", key);
    return false;
  }
  *value = (unsigned)item->valuedouble;
  return true;
}

/* 校验云端配置与实际面板一致；缺省字段不校验 */
static bool display_check_cfg(const char *owner, const cJSON *cfg) {
  const display_info_t *info = display_info();
  if (info == NULL) {
    return false;
  }

  bool present;
  unsigned value;
  if (!display_opt_pixels(cfg, "width", &present, &value)) {
    return false;
  }
  if (present && value != info->width) {
    ESP_LOGE(TAG, "display[%s]: specs.width=%u 与面板 %u 不符", owner, value,
             (unsigned)info->width);
    return false;
  }
  if (!display_opt_pixels(cfg, "height", &present, &value)) {
    return false;
  }
  if (present && value != info->height) {
    ESP_LOGE(TAG, "display[%s]: specs.height=%u 与面板 %u 不符", owner, value,
             (unsigned)info->height);
    return false;
  }

  const cJSON *chip = cJSON_GetObjectItem(cfg, "chip");
  if (chip != NULL) {
    const char *id = display_chip_id();
    if (!cJSON_IsString(chip) || chip->valuestring[0] == '\0' || id == NULL ||
        strncasecmp(id, chip->valuestring, strlen(chip->valuestring)) != 0) {
      ESP_LOGE(TAG, "display[%s]: specs.chip='%s' 与面板 '%s' 不符", owner,
               cJSON_IsString(chip) ? chip->valuestring : "?",
               id != NULL ? id : "unknown");
      return false;
    }
  }

  const cJSON *depth = cJSON_GetObjectItem(cfg, "color_depth");
  if (depth != NULL) {
    const char *actual = display_color_depth();
    if (!cJSON_IsString(depth) || actual == NULL ||
        strcmp(depth->valuestring, actual) != 0) {
      ESP_LOGE(TAG, "display[%s]: specs.color_depth='%s' 不受支持(当前 %s)",
               owner, cJSON_IsString(depth) ? depth->valuestring : "?",
               actual != NULL ? actual : "unknown");
      return false;
    }
  }
  return true;
}

static bool display_probe(periph_device_t *dev, const cJSON *cfg) {
  if (!display_claim_pins(dev->name)) {
    ESP_LOGE(TAG, "面板引脚被其它设备占用");
    return false;
  }
  if (display_init() != ESP_OK) {
    return false;
  }
  return display_check_cfg(dev->name, cfg);
}

static void display_remove(periph_device_t *dev) {
  (void)dev; // 板载固定外设，保持引脚与总线占用
}

static bool display_command(periph_device_t *dev, const cJSON *pl) {
  if (dev == NULL || pl == NULL) {
    return false;
  }
  const cJSON *value = cJSON_GetObjectItem(pl, "value");
  const cJSON *op = cJSON_IsObject(value) ? cJSON_GetObjectItem(value, "op")
                                          : NULL;
  if (!cJSON_IsString(op)) {
    return false;
  }

  if (strcmp(op->valuestring, "selftest") == 0) {
    ESP_LOGW(TAG, "display[%s]: selftest 原地阻塞刷屏", dev->name);
    display_self_test();
    return true;
  }

  ESP_LOGW(TAG, "display[%s]: unknown op '%s'", dev->name, op->valuestring);
  return false;
}

static const periph_ops_t display_ops = {
    .command = display_command,
};

periph_driver_t display_driver = {
    .name = "epd",
    .ops = &display_ops,
    .probe = display_probe,
    .remove = display_remove,
};
