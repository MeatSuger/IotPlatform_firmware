// JD79665（华为 A1）3.98" 768x552 BWRY 墨水屏，transport "epd"。
// 位图 2bpp BWRY、4 px/byte、MSB first；BUSY 低 = 忙。
// 固定走 SPI2_HOST + IOMUX 原生脚 10/11/12；不要改 SPI3（无 IOMUX 脚，走 GPIO
// matrix 会改变面板时序）。代价：与 periph 的 "spi" transport 争 SPI2_HOST。

#include "epd_driver.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "cJSON.h"
#include "common.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"

#include "epd.h"

static const char *TAG = "epd";

static spi_device_handle_t s_spi;
static bool s_bus_ready;
static bool s_pins_claimed;
static esp_pm_lock_handle_t s_pm_lock;
// 上次成功上电的参数（RST 宽度/SPI 时钟），失败时逐项回退
static uint32_t s_rst_low_ms = EPD_RST_LOW_MS;
static uint32_t s_spi_hz = EPD_SPI_HZ;

static inline bool epd_busy(void) { return gpio_get_level(EPD_PIN_BUSY) == 0; }

/* SPI 故障标记：传输失败时置位，不用 ESP_ERROR_CHECK 直接 abort */
static bool s_io_failed;

// ── 互斥 + PM 锁 ────────────────────────────────────────────
// 刷屏、自检、probe/remove 三方会碰同一块面板（配置快照与 MQTT 重连都会触发
// remove+probe），必须串行化：避免 spi_bus_remove_device() 在 transmit 中途释放
// 句柄，或复位脉冲打断 15 s 波形。
static StaticSemaphore_t s_lock_buf;
static SemaphoreHandle_t s_lock;
static portMUX_TYPE s_lock_mux = portMUX_INITIALIZER_UNLOCKED;

static SemaphoreHandle_t epd_mutex(void) {
  portENTER_CRITICAL(&s_lock_mux);
  if (s_lock == NULL) {
    s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
  }
  SemaphoreHandle_t ret = s_lock;
  portEXIT_CRITICAL(&s_lock_mux);
  return ret;
}

static void epd_lock(void) {
  SemaphoreHandle_t mutex = epd_mutex();
  if (mutex == NULL) {
    ESP_LOGE(TAG, "epd mutex unavailable");
    return;
  }
  xSemaphoreTake(mutex, portMAX_DELAY);
}

static void epd_unlock(void) {
  SemaphoreHandle_t mutex = epd_mutex();
  if (mutex != NULL) {
    xSemaphoreGive(mutex);
  }
}

// light sleep（CONFIG_PM_SLP_DISABLE_GPIO）会把 RST/DC/CS 打成睡眠态、并关掉 BUSY
// 的输入与上拉，刷屏期间必须挡住。
static void epd_pm_acquire(void) {
#if CONFIG_PM_ENABLE
  if (s_pm_lock == NULL) {
    esp_err_t err =
        esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "epd", &s_pm_lock);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "pm lock create failed: %s（退化为允许 light sleep）",
               esp_err_to_name(err));
      return;
    }
  }
  esp_err_t err = esp_pm_lock_acquire(s_pm_lock);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "pm lock acquire failed: %s", esp_err_to_name(err));
  }
#endif
}

static void epd_pm_release(void) {
#if CONFIG_PM_ENABLE
  if (s_pm_lock != NULL) {
    esp_err_t err = esp_pm_lock_release(s_pm_lock);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "pm lock release failed: %s", esp_err_to_name(err));
    }
  }
#endif
}

// 面板操作的统一入口/出口（持锁期间禁止 light sleep）
static void epd_enter(void) {
  epd_lock();
  epd_pm_acquire();
}

static void epd_leave(void) {
  epd_pm_release();
  epd_unlock();
}

// ── 引脚与总线 ──────────────────────────────────────────────
static const int s_pins[] = {EPD_PIN_MOSI, EPD_PIN_SCK,  EPD_PIN_CS,
                             EPD_PIN_DC,   EPD_PIN_RST, EPD_PIN_BUSY};

// 面板固定接线：一次性占满引脚，避免被云端 actuator 抢占
static bool epd_pins_claim(const char *owner) {
  if (s_pins_claimed) {
    return true;
  }
  for (size_t i = 0; i < EPD_ARRAY_LEN(s_pins); i++) {
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

static void epd_pins_release(void) {
  for (size_t i = 0; i < EPD_ARRAY_LEN(s_pins); i++) {
    periph_pin_release(s_pins[i]);
  }
  s_pins_claimed = false;
}

static bool epd_gpio_init(void) {
  gpio_config_t out = {
      .pin_bit_mask = (1ULL << EPD_PIN_DC) | (1ULL << EPD_PIN_RST),
      .mode = GPIO_MODE_OUTPUT,
  };
  if (gpio_config(&out) != ESP_OK) {
    ESP_LOGE(TAG, "gpio_config(DC/RST) failed");
    return false;
  }

  // BUSY 常为开漏输出，靠内部上拉
  gpio_config_t in = {
      .pin_bit_mask = (1ULL << EPD_PIN_BUSY),
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,
  };
  if (gpio_config(&in) != ESP_OK) {
    ESP_LOGE(TAG, "gpio_config(BUSY) failed");
    return false;
  }

  gpio_set_level(EPD_PIN_DC, 1);
  gpio_set_level(EPD_PIN_RST, 1);
  return true;
}

// 重新挂设备以切换时钟（SPI device 的频率不可原地修改）
static bool epd_spi_set_clock(uint32_t hz) {
  if (s_spi != NULL) {
    if (spi_bus_remove_device(s_spi) != ESP_OK) {
      s_spi = NULL;
      return false;
    }
    s_spi = NULL;
  }
  spi_device_interface_config_t dev = {
      .clock_speed_hz = (int)hz,
      .mode = 0,
      .spics_io_num = EPD_PIN_CS,
      .queue_size = 1,
  };
  esp_err_t err = spi_bus_add_device(EPD_SPI_HOST, &dev, &s_spi);
  if (err != ESP_OK) {
    s_spi = NULL;
    ESP_LOGE(TAG, "spi_bus_add_device(%u Hz) 失败: %s", (unsigned)hz,
             esp_err_to_name(err));
    return false;
  }
  return true;
}

static bool epd_spi_init(void) {
  spi_bus_config_t bus = {
      .mosi_io_num = EPD_PIN_MOSI,
      .miso_io_num = -1,
      .sclk_io_num = EPD_PIN_SCK,
      .quadwp_io_num = -1,
      .quadhd_io_num = -1,
      .max_transfer_sz = EPD_SPI_CHUNK,
  };
  if (spi_bus_initialize(EPD_SPI_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
    ESP_LOGE(TAG, "spi_bus_initialize(SPI2) 失败");
    return false;
  }

  if (!epd_spi_set_clock(EPD_SPI_HZ)) {
    spi_bus_free(EPD_SPI_HOST);
    return false;
  }
  return true;
}

static void epd_send(const void *buf, size_t len) {
  if (s_io_failed) {
    return;
  }
  spi_transaction_t t = {
      .length = len * 8,
      .tx_buffer = buf,
  };
  esp_err_t err = spi_device_polling_transmit(s_spi, &t);
  if (err != ESP_OK) {
    s_io_failed = true;
    ESP_LOGE(TAG, "spi transmit failed: %s", esp_err_to_name(err));
  }
}

static void epd_cmd(uint8_t cmd) {
  gpio_set_level(EPD_PIN_DC, 0);
  epd_send(&cmd, 1);
  gpio_set_level(EPD_PIN_DC, 1);
}

static void epd_data(uint8_t data) { epd_send(&data, 1); }

// ── BUSY 等待 ───────────────────────────────────────────────
// 两段式：先等断言（命令发出到面板拉低有启动延迟），再等释放。窗口太短会把
// “还没拉低”误判成“已完成”，于是提前发下一条命令/数据。
static bool epd_busy_assert(uint32_t window_ms) {
  const int64_t t0 = esp_timer_get_time();
  while ((esp_timer_get_time() - t0) < (int64_t)window_ms * 1000) {
    if (epd_busy()) {
      return true;
    }
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  return false;
}

// deadline_us 为绝对截止时刻
static bool epd_busy_release(int64_t deadline_us, const char *tag) {
  int64_t last_log = esp_timer_get_time();
  while (epd_busy()) {
    const int64_t now = esp_timer_get_time();
    if (now >= deadline_us) {
      ESP_LOGE(TAG, "[%s] BUSY timeout (level=%d)", tag,
               gpio_get_level(EPD_PIN_BUSY));
      return false;
    }
    if (now - last_log >= 1000000) {
      ESP_LOGW(TAG, "[%s] busy... %u ms left", tag,
               (unsigned)((deadline_us - now) / 1000));
      last_log = now;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  return true;
}

static bool epd_busy_wait_idle(uint32_t timeout_ms, const char *tag) {
  if (!epd_busy()) {
    return true;
  }
  const int64_t t0 = esp_timer_get_time();
  int64_t last_log = t0;
  while (epd_busy()) {
    const int64_t now = esp_timer_get_time();
    if (now - t0 >= (int64_t)timeout_ms * 1000) {
      ESP_LOGE(TAG, "[%s] 面板一直忙（>%u ms, level=%d）", tag,
               (unsigned)timeout_ms, gpio_get_level(EPD_PIN_BUSY));
      return false;
    }
    if (now - last_log >= 1000000) {
      ESP_LOGW(TAG, "[%s] 等待空闲 %u ms", tag, (unsigned)((now - t0) / 1000));
      last_log = now;
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  return true;
}

// 断言 + 释放整套等待；没等到断言即失败（面板可能压根没执行）
static bool epd_busy_cycle(uint32_t assert_ms, uint32_t max_ms,
                           const char *tag) {
  const int64_t t0 = esp_timer_get_time();
  if (!epd_busy_assert(assert_ms)) {
    ESP_LOGW(TAG, "[%s] %u ms 内 BUSY 未拉起（level=%d）", tag,
             (unsigned)assert_ms, gpio_get_level(EPD_PIN_BUSY));
    return false;
  }
  if (!epd_busy_release(t0 + (int64_t)max_ms * 1000, tag)) {
    return false;
  }
  ESP_LOGI(TAG, "[%s] BUSY %u ms", tag,
           (unsigned)((esp_timer_get_time() - t0) / 1000));
  return true;
}

// ── 面板时序与寄存器序列 ────────────────────────────────────
static void epd_hw_reset(uint32_t low_ms) {
  gpio_set_level(EPD_PIN_RST, 1);
  vTaskDelay(pdMS_TO_TICKS(EPD_RST_LOW_MS));
  gpio_set_level(EPD_PIN_RST, 0);
  vTaskDelay(pdMS_TO_TICKS(low_ms));
  gpio_set_level(EPD_PIN_RST, 1);
  vTaskDelay(pdMS_TO_TICKS(EPD_RST_SETTLE_MS));
}

typedef struct {
  uint8_t cmd;
  uint8_t len;
  uint8_t data[6];
} epd_reg_t;

static void epd_write_regs(const epd_reg_t *seq, size_t n) {
  for (size_t i = 0; i < n; i++) {
    epd_cmd(seq[i].cmd);
    for (uint8_t k = 0; k < seq[i].len; k++) {
      epd_data(seq[i].data[k]);
    }
  }
}

// 分辨率（0x61）依赖面板尺寸，夹在两段固定序列中间发送
static const epd_reg_t s_init_pre_res[] = {
    {0xAA, 6, {0x49, 0x55, 0x20, 0x08, 0x09, 0x18}}, // booster / VCOM
    {0x01, 1, {0x3F}},
    {0x00, 2, {0x4B, 0x69}},
    {0x05, 4, {0x40, 0x1F, 0x1F, 0x2C}},
    {0x08, 4, {0x6F, 0x1F, 0x1F, 0x22}},
    {0x06, 4, {0x6F, 0x1F, 0x14, 0x14}},
    {0x03, 4, {0x00, 0x54, 0x00, 0x44}},
    {0x60, 2, {0x02, 0x00}},
    {0x30, 1, {EPD_PLL}},
    {0x50, 1, {0x3F}},
};

static const epd_reg_t s_init_post_res[] = {
    {0x65, 4, {0x10, 0x00, 0x20, 0x00}},
    {0xE3, 1, {0x2F}},
    {0x84, 1, {0x01}},
};

static void epd_set_window(void) {
  epd_cmd(EPD_CMD_PARTIAL_WINDOW);
  epd_data(0x00);
  epd_data(0x00);
  epd_data((EPD_BITMAP_W - 1) >> 8);
  epd_data((EPD_BITMAP_W - 1) & 0xFF);
  epd_data(0x00);
  epd_data(0x00);
  epd_data((EPD_BITMAP_H - 1) >> 8);
  epd_data((EPD_BITMAP_H - 1) & 0xFF);
  epd_data(0x01);
}

// 复位 + 寄存器配置 + 上电；rst_ms 决定唤醒脉冲宽度，spi_hz 决定总线时钟
static bool epd_power_on(uint32_t rst_ms, uint32_t spi_hz) {
  if (!epd_spi_set_clock(spi_hz)) {
    return false;
  }
  epd_hw_reset(rst_ms);
  // 复位后 IC 未必拉低 BUSY，这里只要求回到空闲
  if (!epd_busy_wait_idle(EPD_RESET_IDLE_MS, "post-reset")) {
    return false;
  }
  vTaskDelay(pdMS_TO_TICKS(30));

  epd_write_regs(s_init_pre_res, EPD_ARRAY_LEN(s_init_pre_res));

  epd_cmd(EPD_CMD_RESOLUTION);
  epd_data(EPD_BITMAP_W >> 8);
  epd_data(EPD_BITMAP_W & 0xFF);
  epd_data(EPD_BITMAP_H >> 8);
  epd_data(EPD_BITMAP_H & 0xFF);

  epd_write_regs(s_init_post_res, EPD_ARRAY_LEN(s_init_post_res));
  epd_set_window();

  epd_cmd(EPD_CMD_POWER_ON);
  // 面板正常则 BUSY 拉低 1~2 s；无应答说明没醒或没供电，必须当失败处理
  return epd_busy_cycle(EPD_POWERON_ASSERT_MS, EPD_POWERON_MAX_MS, "power-on");
}

// 上电无应答时逐项回退：加长复位（DSLP 唤不醒）→ 再降 SPI 时钟（信号完整性），
// 成功后钉住这组参数。
typedef struct {
  uint32_t rst_ms;
  uint32_t spi_hz;
  const char *note;
} epd_try_t;

static const epd_try_t s_init_tries[] = {
    {EPD_RST_WAKE_MS, EPD_SPI_HZ, "复位 200 ms"},
    {EPD_RST_WAKE_MS, EPD_SPI_HZ / 10, "复位 200 ms + SPI 1 MHz"},
};

static bool epd_init(void) {
  if (epd_power_on(s_rst_low_ms, s_spi_hz)) {
    return true;
  }

  ESP_LOGW(TAG, "上电无 BUSY 应答（RST %u ms, SPI %u Hz），开始逐项回退",
           (unsigned)s_rst_low_ms, (unsigned)s_spi_hz);

  for (size_t i = 0; i < EPD_ARRAY_LEN(s_init_tries); i++) {
    if (!epd_power_on(s_init_tries[i].rst_ms, s_init_tries[i].spi_hz)) {
      continue;
    }
    s_rst_low_ms = s_init_tries[i].rst_ms;
    s_spi_hz = s_init_tries[i].spi_hz;
    ESP_LOGW(TAG, "%s 后上电成功（RST %u ms, SPI %u Hz），后续沿用这组参数",
             s_init_tries[i].note, (unsigned)s_rst_low_ms,
             (unsigned)s_spi_hz);
    return true;
  }

  ESP_LOGE(TAG,
           "面板无响应（20/200 ms 复位 × 10/1 MHz 都不拉起 BUSY）：供电、"
           "接线或面板已失效");
  return false;
}

// R12H 整屏刷新，实测约 15.2 s（波形烧在芯片 MTP 里，无法加速）。
// 从 0x12 起至少等满 EPD_REFRESH_MIN_MS 才掉电，避免切在波形尾巴上。
static bool epd_refresh(void) {
  epd_set_window();
  epd_cmd(EPD_CMD_REFRESH);
  epd_data(0x00);

  const int64_t t0 = esp_timer_get_time();
  bool ok = true;
  bool asserted = epd_busy_assert(EPD_REFRESH_ASSERT_MS);

  if (asserted) {
    ok = epd_busy_release(t0 + (int64_t)EPD_REFRESH_MAX_MS * 1000, "refresh");
  } else {
    ESP_LOGW(TAG,
             "refresh BUSY 未断言(%d ms 内)：无法确认波形结束，盲等满 %d ms"
             "（检查 BUSY 接线/上拉/面板型号）",
             EPD_REFRESH_ASSERT_MS, EPD_REFRESH_MIN_MS);
  }

  uint32_t waited = (uint32_t)((esp_timer_get_time() - t0) / 1000);
  if (waited < EPD_REFRESH_MIN_MS) {
    vTaskDelay(pdMS_TO_TICKS(EPD_REFRESH_MIN_MS - waited));
    waited = EPD_REFRESH_MIN_MS;
  }

  ESP_LOGI(TAG, "refresh %s in %u ms (assert=%d)", ok ? "done" : "TIMEOUT",
           (unsigned)waited, (int)asserted);

  epd_cmd(EPD_CMD_POWER_OFF);
  epd_data(0x00);
  epd_busy_cycle(EPD_BUSY_ASSERT_MS, EPD_POWEROFF_MAX_MS, "power-off");
  return ok;
}

static void epd_deep_sleep(void) {
  epd_cmd(EPD_CMD_DEEP_SLEEP);
  epd_data(EPD_DEEP_SLEEP_CODE);
  vTaskDelay(pdMS_TO_TICKS(200));
}

// 失败兜底：复位回已知状态再 DSLP，避免下一张从被打断的波形上起步
static void epd_recover(void) {
  ESP_LOGW(TAG, "recover: hardware reset + DSLP");
  epd_hw_reset(EPD_RST_LOW_MS);
  epd_busy_wait_idle(EPD_RESET_IDLE_MS, "recover");
  epd_deep_sleep();
}

// ── 自检图案（流式生成，不占 106KB 内存） ───────────────────
typedef struct {
  size_t pos;
} pattern_t;

#if !EPD_SELFTEST_GRAY
static int pattern_bars(void *ctx, uint8_t *buf, size_t len) {
  pattern_t *p = (pattern_t *)ctx;
  const size_t stride = EPD_BITMAP_W / 4;

  for (size_t i = 0; i < len; i++) {
    size_t xb = (p->pos + i) % stride;
    uint8_t packed = 0;
    for (int k = 0; k < 4; k++) {
      int color = (int)((xb * 4 + k) / (EPD_BITMAP_W / 4));
      if (color > 3) {
        color = 3;
      }
      packed |= (uint8_t)(color & 0x03) << (6 - k * 2);
    }
    buf[i] = packed;
  }
  p->pos += len;
  return (int)len;
}
#else
// 16 条竖带，Bayer 4x4 抖动模拟灰阶（面板无原生灰度）
static const uint8_t s_bayer4[4][4] = {
    {0, 8, 2, 10},
    {12, 4, 14, 6},
    {3, 11, 1, 9},
    {15, 7, 13, 5},
};

static int pattern_gray(void *ctx, uint8_t *buf, size_t len) {
  pattern_t *p = (pattern_t *)ctx;
  const size_t stride = EPD_BITMAP_W / 4;
  const int bands = 16;
  const int band_w = EPD_BITMAP_W / bands;

  for (size_t i = 0; i < len; i++) {
    size_t idx = p->pos + i;
    size_t y = idx / stride;
    size_t xb = idx % stride;
    const uint8_t *brow = s_bayer4[y & 3];
    uint8_t packed = 0;
    for (int k = 0; k < 4; k++) {
      int x = (int)(xb * 4) + k;
      int level = (255 * (bands - 1 - x / band_w)) / (bands - 1);
      int color = (level >= brow[x & 3] * 16 + 8) ? EPD_COLOR_BLACK
                                                  : EPD_COLOR_WHITE;
      packed |= (uint8_t)(color & 0x03) << (6 - k * 2);
    }
    buf[i] = packed;
  }
  p->pos += len;
  return (int)len;
}
#endif

// ── 刷屏 ────────────────────────────────────────────────────
// GPIO / SPI / 引脚占用只做一次
static bool epd_bus_init(const char *owner) {
  if (s_bus_ready) {
    return true;
  }
  if (!epd_pins_claim(owner)) {
    return false;
  }
  if (!epd_gpio_init()) {
    return false;
  }
  epd_hw_reset(EPD_RST_LOW_MS); // 把面板从上次 DSLP 唤醒
  if (!epd_spi_init()) {
    epd_pins_release();
    return false;
  }
  s_bus_ready = true;
  return true;
}

// 写 GRAM + 整屏刷新。调用方已持锁；返回 ESP_OK 表示面板已完成刷新".
static esp_err_t epd_draw(epd_bitmap_read_t read_cb, void *ctx, uint8_t *chunk) {
  if (!epd_init()) {
    ESP_LOGE(TAG, "面板初始化/上电失败，放弃本次刷屏（不写数据）");
    return ESP_FAIL;
  }

  epd_set_window();
  // 忙时写 GRAM 会被 IC 丢弃
  if (!epd_busy_wait_idle(EPD_IDLE_TIMEOUT_MS, "pre-data")) {
    return ESP_FAIL;
  }
  epd_cmd(EPD_CMD_DATA_START);

  size_t sent = 0;
  const int64_t t_data = esp_timer_get_time();
  while (sent < EPD_BITMAP_SIZE) {
    const size_t left = EPD_BITMAP_SIZE - sent;
    const size_t want = (left > EPD_SPI_CHUNK) ? EPD_SPI_CHUNK : left;
    int n = read_cb(ctx, chunk, want);
    if (n <= 0 || (size_t)n > want) {
      ESP_LOGE(TAG, "bitmap source short read at %u/%d", (unsigned)sent,
               EPD_BITMAP_SIZE);
      return ESP_FAIL;
    }
    epd_send(chunk, (size_t)n);
    sent += (size_t)n;
  }
  ESP_LOGI(TAG, "data sent: %u bytes, %u ms", (unsigned)sent,
           (unsigned)((esp_timer_get_time() - t_data) / 1000));

  if (!epd_busy_wait_idle(EPD_IDLE_TIMEOUT_MS, "post-data")) {
    return ESP_FAIL;
  }
  if (!epd_refresh()) {
    return ESP_FAIL;
  }
  if (s_io_failed) {
    ESP_LOGE(TAG, "SPI 传输失败，本次刷屏作废");
    return ESP_FAIL;
  }
  return ESP_OK;
}

esp_err_t epd_show_bitmap_stream(epd_bitmap_read_t read_cb, void *ctx) {
  if (read_cb == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  epd_enter();
  s_io_failed = false;

  // 只用一个分块做 DMA，不需要 106KB 连续内存
  uint8_t *chunk = heap_caps_malloc(EPD_SPI_CHUNK, MALLOC_CAP_DMA);
  bool bus_ready = epd_bus_init("epd");
  bool touched = false;
  esp_err_t err = ESP_FAIL;

  if (!bus_ready) {
    ESP_LOGE(TAG, "面板总线初始化失败（引脚被其它设备占用？）");
  } else if (chunk == NULL) {
    ESP_LOGE(TAG, "chunk alloc failed (%d bytes)", EPD_SPI_CHUNK);
    err = ESP_ERR_NO_MEM;
  } else {
    ESP_LOGI(TAG, "show bitmap: %d bytes", EPD_BITMAP_SIZE);
    touched = true; // 已经动过面板，失败要复位回已知状态
    err = epd_draw(read_cb, ctx, chunk);
  }

  if (touched) {
    if (err == ESP_OK) {
      epd_deep_sleep();
    } else {
      epd_recover();
    }
  }
  if (chunk != NULL) {
    heap_caps_free(chunk);
    chunk = NULL;
  }
  epd_leave();

  ESP_LOGI(TAG, "show bitmap %s", err == ESP_OK ? "done" : "FAILED");
  return err;
}

// 自检：与传图共用同一条通路
void epd_app(void) {
  ESP_LOGI(TAG, "JD79665 A1, %dx%d, PLL=0x%02X", EPD_BITMAP_W, EPD_BITMAP_H,
           EPD_PLL);

  pattern_t gen = {.pos = 0};
  esp_err_t err;

#if EPD_SELFTEST_GRAY
  ESP_LOGI(TAG, "self-test: 16 x Bayer 4x4 grayscale bars (100%%..0%% black)");
  err = epd_show_bitmap_stream(pattern_gray, &gen);
#else
  ESP_LOGI(TAG, "self-test: color bars black | white | yellow | red");
  err = epd_show_bitmap_stream(pattern_bars, &gen);
#endif

  ESP_LOGI(TAG, "self-test %s", err == ESP_OK ? "done" : "FAILED");
}

// ── periph transport ────────────────────────────────────────
static bool epd_probe(periph_device_t *dev, const cJSON *cfg) {
  (void)cfg; // 引脚固定，无配置字段
  epd_enter();
  bool ok = epd_bus_init(dev->name);
  epd_leave();
  return ok;
}

static void epd_remove(periph_device_t *dev) {
  (void)dev;

  // 刷屏进行中不拆总线（会让 transmit 拿到已释放的句柄），保持引脚占用，
  // 紧随其后的 probe 直接返回成功。
  if (xSemaphoreTake(epd_mutex(), pdMS_TO_TICKS(1000)) != pdTRUE) {
    ESP_LOGW(TAG, "remove 跳过：刷屏进行中，保持总线与引脚占用");
    return;
  }

  // 先移除 SPI device，再释放 SPI bus
  if (s_spi != NULL) {
    esp_err_t err = spi_bus_remove_device(s_spi);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "spi_bus_remove_device 失败: %s", esp_err_to_name(err));
      epd_unlock();
      return;
    }
    s_spi = NULL;
  }

  if (s_bus_ready) {
    esp_err_t err = spi_bus_free(EPD_SPI_HOST);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "spi_bus_free 失败: %s", esp_err_to_name(err));
      epd_unlock();
      return;
    }
    s_bus_ready = false;
  }

  epd_pins_release();

  // 清掉 GPIO 配置，避免重新 probe 时残留
  gpio_reset_pin(EPD_PIN_DC);
  gpio_reset_pin(EPD_PIN_RST);
  gpio_reset_pin(EPD_PIN_BUSY);

  epd_unlock();
}

static bool epd_command(periph_device_t *dev, const cJSON *pl) {
  if (dev == NULL || pl == NULL) {
    return false;
  }

  const cJSON *value = cJSON_GetObjectItem(pl, "value");
  if (!cJSON_IsObject(value)) {
    return false;
  }
  const cJSON *op = cJSON_GetObjectItem(value, "op");
  if (!cJSON_IsString(op)) {
    return false;
  }

  if (strcmp(op->valuestring, "selftest") == 0) {
    ESP_LOGW(TAG, "epd[%s]: selftest 原地阻塞刷新约 15s", dev->name);
    epd_app();
    return true;
  }

  ESP_LOGW(TAG, "epd[%s]: unknown op '%s'", dev->name, op->valuestring);
  return false;
}

static const periph_ops_t epd_ops = {
    .command = epd_command,
};

periph_driver_t epd_driver = {
    .name = "epd",
    .ops = &epd_ops,
    .probe = epd_probe,
    .remove = epd_remove,
};
