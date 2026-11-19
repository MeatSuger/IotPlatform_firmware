// JD79665(华为 A1)3.98" 768x552 四色 BWRY 适配层。
// 命令序列(移植自 Hwawei-epd/jd6xxx,已在实屏验证)：
//   复位 → 0xAA/0x01/0x00(4B,69)/0x05/0x08/0x06/0x03/0x60/0x30(08)/0x50(3F)
//   → 0x61(768x552) → 0x65/0xE3(2F)/0x84(01) → 0x83 整屏窗口 → 0x04 上电
//   刷屏 0x10 + 整帧 2bpp → 0x12(0x00) → 0x02 掉电；深睡 0x07+0xA5。
// BUSY 为「低 = 忙」(开漏,需要上拉)。

#include <string.h>

#include "display.h"
#include "display_ctrl.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"

static const char *TAG = "jd79665";

#define JD_WIDTH 768
#define JD_HEIGHT 552
#define JD_ROW_BYTES (JD_WIDTH / 4)               /* 2bpp：192 字节/行 */
#define JD_FRAME_BYTES (JD_ROW_BYTES * JD_HEIGHT) /* 105984 */

/* 复位低电平：面板进过 DSLP,20 ms 脉冲唤不醒 */
#define JD_RST_LOW_MS 200

typedef struct {
  uint8_t cmd;
  uint8_t len;
  uint8_t data[6];
} jd_reg_t;

static const jd_reg_t s_init_pre[] = {
    {0xAA, 6, {0x49, 0x55, 0x20, 0x08, 0x09, 0x18}}, // booster / VCOM
    {0x01, 1, {0x3F}},
    {0x00, 2, {0x4B, 0x69}}, // PSR
    {0x05, 4, {0x40, 0x1F, 0x1F, 0x2C}},
    {0x08, 4, {0x6F, 0x1F, 0x1F, 0x22}},
    {0x06, 4, {0x6F, 0x1F, 0x14, 0x14}},
    {0x03, 4, {0x00, 0x54, 0x00, 0x44}},
    {0x60, 2, {0x02, 0x00}},
    {0x30, 1, {0x08}}, // PLL 12.5 Hz
    {0x50, 1, {0x3F}},
};

static const jd_reg_t s_init_post[] = {
    {0x65, 4, {0x10, 0x00, 0x20, 0x00}},
    {0xE3, 1, {0x2F}},
    {0x84, 1, {0x01}},
};

static uint8_t *s_frame;

static void jd_reset(void) {
  gpio_set_level(DISPLAY_PIN_RST, 1);
  vTaskDelay(pdMS_TO_TICKS(20));
  gpio_set_level(DISPLAY_PIN_RST, 0);
  vTaskDelay(pdMS_TO_TICKS(JD_RST_LOW_MS));
  gpio_set_level(DISPLAY_PIN_RST, 1);
  vTaskDelay(pdMS_TO_TICKS(20));
}

static void jd_write_regs(const jd_reg_t *seq, size_t n) {
  for (size_t i = 0; i < n; i++) {
    display_bus_cmd(seq[i].cmd);
    for (uint8_t k = 0; k < seq[i].len; k++) {
      display_bus_data(seq[i].data[k]);
    }
  }
}

static void jd_set_window(void) {
  display_bus_cmd(0x83);
  display_bus_data(0x00);
  display_bus_data(0x00);
  display_bus_data((JD_WIDTH - 1) >> 8);
  display_bus_data((JD_WIDTH - 1) & 0xFF);
  display_bus_data(0x00);
  display_bus_data(0x00);
  display_bus_data((JD_HEIGHT - 1) >> 8);
  display_bus_data((JD_HEIGHT - 1) & 0xFF);
  display_bus_data(0x01);
}

static esp_err_t jd_init(void) {
  if (s_frame == NULL) {
    s_frame = heap_caps_malloc(JD_FRAME_BYTES, MALLOC_CAP_DMA);
    if (s_frame == NULL) {
      ESP_LOGE(TAG, "帧缓冲分配失败(%d B)", JD_FRAME_BYTES);
      return ESP_ERR_NO_MEM;
    }
  }
  memset(s_frame, 0x00, JD_FRAME_BYTES);

  jd_reset();
  if (!display_bus_wait_busy(false, 5000, "post-reset")) {
    return ESP_ERR_NOT_FOUND;
  }
  vTaskDelay(pdMS_TO_TICKS(30));

  jd_write_regs(s_init_pre, sizeof(s_init_pre) / sizeof(s_init_pre[0]));
  display_bus_cmd(0x61);
  display_bus_data(JD_WIDTH >> 8);
  display_bus_data(JD_WIDTH & 0xFF);
  display_bus_data(JD_HEIGHT >> 8);
  display_bus_data(JD_HEIGHT & 0xFF);
  jd_write_regs(s_init_post, sizeof(s_init_post) / sizeof(s_init_post[0]));
  jd_set_window();

  display_bus_cmd(0x04); // power on
  return display_bus_wait_busy(false, 10000, "power-on") ? ESP_OK
                                                         : ESP_FAIL;
}

// 原生格式与逻辑位图一致(2bpp BWRY),整行拷贝
static void jd_store_row(uint16_t y, const uint8_t *row) {
  if (y < JD_HEIGHT) {
    memcpy(s_frame + (size_t)y * JD_ROW_BYTES, row, JD_ROW_BYTES);
  }
}

static esp_err_t jd_flush(void) {
  jd_set_window();
  display_bus_cmd(0x10); // data start
  display_bus_write(s_frame, JD_FRAME_BYTES);

  display_bus_cmd(0x12); // refresh
  display_bus_data(0x00);
  const bool ok = display_bus_wait_busy(false, 40000, "refresh");

  display_bus_cmd(0x02); // power off
  display_bus_data(0x00);
  display_bus_wait_busy(false, 10000, "power-off");
  return ok ? ESP_OK : ESP_FAIL;
}

static void jd_sleep(void) {
  display_bus_cmd(0x07);
  display_bus_data(0xA5);
  vTaskDelay(pdMS_TO_TICKS(200));
}

const display_ctrl_t display_ctrl_jd79665_3in98 = {
    .name = "JD79665 3.98\" BWRY",
    .width = JD_WIDTH,
    .height = JD_HEIGHT,
    .color_mask = DISPLAY_COLOR_BIT(DISPLAY_COLOR_BLACK) |
                  DISPLAY_COLOR_BIT(DISPLAY_COLOR_WHITE) |
                  DISPLAY_COLOR_BIT(DISPLAY_COLOR_YELLOW) |
                  DISPLAY_COLOR_BIT(DISPLAY_COLOR_RED),
    .busy_high_means_busy = false,
    .init = jd_init,
    .store_row = jd_store_row,
    .flush = jd_flush,
    .sleep = jd_sleep,
};
