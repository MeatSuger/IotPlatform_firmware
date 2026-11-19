// SSD1683 4.2" 400x300 BWR(黑/白/红)。命令序列：
//   复位 → 0x12 → 0x21/0x3C → 0x11/0x44/0x45/0x4E/0x4F 窗口
//   → 0x24(BW)/0x26(RED) 双平面 → 0x22=0xF7 → 0x20 → 等 BUSY(高=忙)。

#include <string.h>

#include "display.h"
#include "display_ctrl.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"

static const char *TAG = "ssd1683";

#define SSD_WIDTH 400
#define SSD_HEIGHT 300
#define SSD_ROW_BYTES (SSD_WIDTH / 8)  /* 1bpp 每行 50 字节 */
#define SSD_PLANE_BYTES (SSD_ROW_BYTES * SSD_HEIGHT) /* 15000 */

/* 0x22：0xF7 = Load LUT(3-color) + DISPLAY(3-color)；0xFF 是黑白模式 */
#define SSD_REFRESH_ARG 0xF7
#define SSD_REFRESH_TIMEOUT_MS 30000

static uint8_t *s_bw;  /* BW 平面：1=白 0=黑 */
static uint8_t *s_red; /* RED 平面：1=红 */

static void ssd_reset(void) {
  gpio_set_level(DISPLAY_PIN_RST, 1);
  vTaskDelay(pdMS_TO_TICKS(100));
  gpio_set_level(DISPLAY_PIN_RST, 0);
  vTaskDelay(pdMS_TO_TICKS(2));
  gpio_set_level(DISPLAY_PIN_RST, 1);
  vTaskDelay(pdMS_TO_TICKS(100));
}

// 0x11 数据自增 + 0x44/0x45 窗口 + 0x4E/0x4F 计数器归零
static void ssd_set_window(void) {
  display_bus_cmd(0x11); // Data Entry Mode
  display_bus_data(0x03); // X increment, Y increment

  display_bus_cmd(0x44); // RAM X range
  display_bus_data(0x00);
  display_bus_data((SSD_WIDTH - 1) / 8);

  display_bus_cmd(0x45); // RAM Y range
  display_bus_data(0x00);
  display_bus_data(0x00);
  display_bus_data((SSD_HEIGHT - 1) & 0xFF);
  display_bus_data(((SSD_HEIGHT - 1) >> 8) & 0xFF);

  display_bus_cmd(0x4E); // X counter
  display_bus_data(0x00);

  display_bus_cmd(0x4F); // Y counter
  display_bus_data(0x00);
  display_bus_data(0x00);
}

static esp_err_t ssd_init(void) {
  if (s_bw == NULL || s_red == NULL) {
    /* 两个平面必须同生同灭：只成功一个时下次 memset 会踩 NULL */
    uint8_t *bw = heap_caps_malloc(SSD_PLANE_BYTES, MALLOC_CAP_DMA);
    uint8_t *red = heap_caps_malloc(SSD_PLANE_BYTES, MALLOC_CAP_DMA);
    if (bw == NULL || red == NULL) {
      heap_caps_free(bw);
      heap_caps_free(red);
      s_bw = NULL;
      s_red = NULL;
      ESP_LOGE(TAG, "平面缓冲分配失败(%d B × 2)", SSD_PLANE_BYTES);
      return ESP_ERR_NO_MEM;
    }
    s_bw = bw;
    s_red = red;
  }
  memset(s_bw, 0x00, SSD_PLANE_BYTES);
  memset(s_red, 0x00, SSD_PLANE_BYTES);

  ssd_reset();
  display_bus_wait_busy(true, 5000, "reset");

  display_bus_cmd(0x12); // 软复位
  display_bus_wait_busy(true, 5000, "soft-reset");

  // 0x21：A[7:4]=Red RAM,A[3:0]=BW。三色屏必须 Normal(0x00),0x40 会让红平面恒 0
  display_bus_cmd(0x21);
  display_bus_data(0x00);
  display_bus_data(0x00);

  display_bus_cmd(0x3C); // Border Waveform
  display_bus_data(0x05);

  ssd_set_window();
  display_bus_wait_busy(true, 5000, "init");
  return ESP_OK;
}

// 逻辑行 → 双平面：黑 BW=0/RED=0；白 BW=1/RED=0；红/黄 两个平面都置 1(只给 RED 会判成黑)
static void ssd_store_row(uint16_t y, const uint8_t *row) {
  if (y >= SSD_HEIGHT) {
    return;
  }
  uint8_t *bw = s_bw + (size_t)y * SSD_ROW_BYTES;
  uint8_t *red = s_red + (size_t)y * SSD_ROW_BYTES;
  memset(bw, 0x00, SSD_ROW_BYTES);
  memset(red, 0x00, SSD_ROW_BYTES);

  for (uint16_t x = 0; x < SSD_WIDTH; x++) {
    const uint8_t color = (row[x >> 2] >> (6 - 2 * (x & 3))) & 0x03;
    const uint8_t bit = (uint8_t)(0x80 >> (x & 7));
    if (color == DISPLAY_COLOR_WHITE) {
      bw[x >> 3] |= bit;
    } else if (color == DISPLAY_COLOR_RED || color == DISPLAY_COLOR_YELLOW) {
      bw[x >> 3] |= bit;
      red[x >> 3] |= bit;
    }
  }
}

static esp_err_t ssd_flush(void) {
  // 每个平面前重设窗口(写完一平面上计数器停在末尾/回绕)
  ssd_set_window();
  display_bus_cmd(0x24); // Write Black/White RAM
  display_bus_write(s_bw, SSD_PLANE_BYTES);

  ssd_set_window();
  display_bus_cmd(0x26); // Write RED RAM
  display_bus_write(s_red, SSD_PLANE_BYTES);

  display_bus_cmd(0x22); // Display Update Control 2
  display_bus_data(SSD_REFRESH_ARG);
  display_bus_cmd(0x20); // Activate Display Update Sequence

  return display_bus_wait_busy(true, SSD_REFRESH_TIMEOUT_MS, "refresh")
             ? ESP_OK
             : ESP_FAIL;
}

static void ssd_sleep(void) {
  display_bus_cmd(0x10); // Deep Sleep Mode
  display_bus_data(0x01);
  vTaskDelay(pdMS_TO_TICKS(200));
}

const display_ctrl_t display_ctrl_ssd1683_4in2 = {
    .name = "SSD1683 4.2\" BWR",
    .width = SSD_WIDTH,
    .height = SSD_HEIGHT,
    .color_mask = DISPLAY_COLOR_BIT(DISPLAY_COLOR_BLACK) |
                  DISPLAY_COLOR_BIT(DISPLAY_COLOR_WHITE) |
                  DISPLAY_COLOR_BIT(DISPLAY_COLOR_RED),
    .busy_high_means_busy = true,
    .init = ssd_init,
    .store_row = ssd_store_row,
    .flush = ssd_flush,
    .sleep = ssd_sleep,
};
