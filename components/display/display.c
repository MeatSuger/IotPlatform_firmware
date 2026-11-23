// 显示调度层：SPI 总线、互斥、PM 锁、BUSY 等待,按逻辑 2bpp 流式喂给适配层。

#include "display.h"

#include "display_ctrl.h"
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

static const char *TAG = "display";

/* 面板自动扫描：按顺序探测,第一个初始化成功即当前面板。
 * 新增面板在此登记；两颗都能初始化时需给 init() 加可区分签名。 */
static const display_ctrl_t *const s_ctrls[] = {
    &display_ctrl_ssd1683_4in2,
    &display_ctrl_jd79665_3in98,
};
static const display_ctrl_t *s_ctrl; /* 探测结果 */

/* BUSY 观察窗：窗口内没忙按已完成处理 */
#define DISPLAY_BUSY_APPEAR_MS 300
#define DISPLAY_INIT_TIMEOUT_MS 10000
#define DISPLAY_REFRESH_TIMEOUT_MS 60000

static spi_device_handle_t s_spi;
static bool s_bus_ready;
static bool s_inited;
static esp_pm_lock_handle_t s_pm_lock;
static SemaphoreHandle_t s_lock;

static uint8_t *s_row_buf;

static bool s_bus_failed;

static SemaphoreHandle_t display_mutex(void) {
  if (s_lock == NULL) {
    s_lock = xSemaphoreCreateMutex();
  }
  return s_lock;
}

/* 总线原语(供适配层调用) */
static void display_bus_transmit(const void *buf, size_t len) {
  if (s_bus_failed) {
    return;
  }
  spi_transaction_t t = {.length = len * 8, .tx_buffer = buf};
  const esp_err_t err = spi_device_polling_transmit(s_spi, &t);
  if (err != ESP_OK) {
    s_bus_failed = true;
    ESP_LOGE(TAG, "SPI 传输失败: %s", esp_err_to_name(err));
  }
}

void display_bus_cmd(uint8_t cmd) {
  gpio_set_level(DISPLAY_PIN_DC, 0); // DC 低 = 命令
  display_bus_transmit(&cmd, 1);
  gpio_set_level(DISPLAY_PIN_DC, 1);
}

void display_bus_data(uint8_t data) { display_bus_transmit(&data, 1); }

void display_bus_write(const void *buf, size_t len) {
  // 分块发送(单笔 ≤ 4 KB)
  const uint8_t *p = (const uint8_t *)buf;
  while (len > 0 && !s_bus_failed) {
    const size_t chunk = (len > 4096) ? 4096 : len;
    display_bus_transmit(p, chunk);
    p += chunk;
    len -= chunk;
  }
}

bool display_bus_wait_busy(bool busy_high, uint32_t timeout_ms,
                           const char *tag) {
  const int busy_level = busy_high ? 1 : 0;
  const int64_t t0 = esp_timer_get_time();
  const int64_t appear_deadline = t0 + (int64_t)DISPLAY_BUSY_APPEAR_MS * 1000;
  int64_t last_log = t0;
  bool saw_busy = false;
  uint32_t elapsed_ms = 0;

  for (;;) {
    if (gpio_get_level(DISPLAY_PIN_BUSY) == busy_level) {
      saw_busy = true;
    } else if (saw_busy) {
      break;     }

    const int64_t now = esp_timer_get_time();
    elapsed_ms = (uint32_t)((now - t0) / 1000);

    if (!saw_busy && now >= appear_deadline) {
      return true; // 没忙过：按已完成处理
    }
    if (now - t0 >= (int64_t)timeout_ms * 1000) {
      ESP_LOGE(TAG, "[%s] BUSY 超时(>%u ms, level=%d)", tag,
               (unsigned)timeout_ms, gpio_get_level(DISPLAY_PIN_BUSY));
      return false;
    }
    if (saw_busy && now - last_log >= 1000000) {
      ESP_LOGW(TAG, "[%s] busy... %u ms", tag, (unsigned)elapsed_ms);
      last_log = now;
    }
    vTaskDelay(1); // 至少 1 tick：pdMS_TO_TICKS(<10)=0 会饿死 idle 触发 WDT
  }

  ESP_LOGI(TAG, "[%s] BUSY %u ms", tag, (unsigned)elapsed_ms);
  return true;
}

/* 引脚与总线 */
static bool display_gpio_init(void) {
  gpio_config_t out = {
      .pin_bit_mask = (1ULL << DISPLAY_PIN_DC) | (1ULL << DISPLAY_PIN_RST),
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  if (gpio_config(&out) != ESP_OK) {
    ESP_LOGE(TAG, "gpio_config(DC/RST) failed");
    return false;
  }
  gpio_set_drive_capability(DISPLAY_PIN_DC, GPIO_DRIVE_CAP_2);
  gpio_set_drive_capability(DISPLAY_PIN_RST, GPIO_DRIVE_CAP_2);

  // JD79665 开漏需上拉,SSD1683 推挽不受影响；复位后按空闲电平判极性
  gpio_config_t in = {
      .pin_bit_mask = (1ULL << DISPLAY_PIN_BUSY),
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  if (gpio_config(&in) != ESP_OK) {
    ESP_LOGE(TAG, "gpio_config(BUSY) failed");
    return false;
  }

  gpio_set_level(DISPLAY_PIN_DC, 1);
  gpio_set_level(DISPLAY_PIN_RST, 1);
  return true;
}

static bool display_spi_init(void) {
  spi_bus_config_t bus = {
      .mosi_io_num = DISPLAY_PIN_MOSI,
      .miso_io_num = -1,
      .sclk_io_num = DISPLAY_PIN_SCK,
      .quadwp_io_num = -1,
      .quadhd_io_num = -1,
      .max_transfer_sz = 4096,
  };
  if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
    ESP_LOGE(TAG, "spi_bus_initialize(SPI2) failed");
    return false;
  }

  spi_device_interface_config_t dev = {
      .clock_speed_hz = DISPLAY_SPI_HZ,
      .mode = 0,
      .spics_io_num = DISPLAY_PIN_CS,
      .queue_size = 1,
      .cs_ena_pretrans = 1,
      .cs_ena_posttrans = 2,
  };
  if (spi_bus_add_device(SPI2_HOST, &dev, &s_spi) != ESP_OK) {
    ESP_LOGE(TAG, "spi_bus_add_device failed");
    spi_bus_free(SPI2_HOST);
    return false;
  }
  gpio_set_drive_capability(DISPLAY_PIN_CS, GPIO_DRIVE_CAP_2);
  gpio_set_drive_capability(DISPLAY_PIN_MOSI, GPIO_DRIVE_CAP_2);
  gpio_set_drive_capability(DISPLAY_PIN_SCK, GPIO_DRIVE_CAP_2);
  return true;
}

static void display_pm_lock(void) {
#if CONFIG_PM_ENABLE
  if (s_pm_lock == NULL) {
    if (esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "display", &s_pm_lock) !=
        ESP_OK) {
      ESP_LOGW(TAG, "pm lock create failed(退化为允许 light sleep)");
      return;
    }
  }
  esp_pm_lock_acquire(s_pm_lock);
#endif
}

static void display_pm_unlock(void) {
#if CONFIG_PM_ENABLE
  if (s_pm_lock != NULL) {
    esp_pm_lock_release(s_pm_lock);
  }
#endif
}

/* 对外 API */
const display_info_t *display_info(void) {
  static display_info_t info;
  if (s_ctrl == NULL) {
    return NULL;
  }
  info.width = s_ctrl->width;
  info.height = s_ctrl->height;
  info.ctrl = s_ctrl->name;
  return &info;
}

size_t display_frame_bytes(void) {
  if (s_ctrl == NULL) {
    return 0;
  }
  return (size_t)s_ctrl->width * s_ctrl->height / 4;
}

const char *display_chip_id(void) {
  return (s_ctrl != NULL) ? s_ctrl->name : NULL;
}

const char *display_color_depth(void) {
  if (s_ctrl == NULL) {
    return NULL;
  }
  if (s_ctrl->color_mask & DISPLAY_COLOR_BIT(DISPLAY_COLOR_YELLOW)) {
    return "bwry";
  }
  if (s_ctrl->color_mask & DISPLAY_COLOR_BIT(DISPLAY_COLOR_RED)) {
    return "bwr";
  }
  return "bw";
}

static esp_err_t display_bringup(void) {
  if (!s_bus_ready) {
    if (!display_spi_init() || !display_gpio_init()) {
      return ESP_FAIL;
    }
    s_bus_ready = true;
  }

  // 复位后按 BUSY 空闲电平决定探测顺序
  gpio_set_level(DISPLAY_PIN_RST, 1);
  vTaskDelay(pdMS_TO_TICKS(20));
  gpio_set_level(DISPLAY_PIN_RST, 0);
  vTaskDelay(pdMS_TO_TICKS(20));
  gpio_set_level(DISPLAY_PIN_RST, 1);
  vTaskDelay(pdMS_TO_TICKS(200));
  const bool idle_level = gpio_get_level(DISPLAY_PIN_BUSY) != 0;
  ESP_LOGI(TAG, "BUSY 空闲电平=%d", (int)idle_level);

  const display_ctrl_t *order[sizeof(s_ctrls) / sizeof(s_ctrls[0])];
  size_t n = 0;
  for (size_t i = 0; i < sizeof(s_ctrls) / sizeof(s_ctrls[0]); i++) {
    if (s_ctrls[i]->busy_high_means_busy != idle_level) {
      order[n++] = s_ctrls[i];
    }
  }
  for (size_t i = 0; i < sizeof(s_ctrls) / sizeof(s_ctrls[0]); i++) {
    if (s_ctrls[i]->busy_high_means_busy == idle_level) {
      order[n++] = s_ctrls[i];
    }
  }

  esp_err_t err = ESP_ERR_NOT_FOUND;
  display_pm_lock();
  for (size_t i = 0; i < n; i++) {
    err = order[i]->init();
    if (err == ESP_OK) {
      s_ctrl = order[i];
      break;
    }
    ESP_LOGW(TAG, "探测 %s 无响应(%s)", order[i]->name,
             esp_err_to_name(err));
  }
  display_pm_unlock();
  if (s_ctrl == NULL) {
    ESP_LOGE(TAG, "未探测到面板控制器");
    return err;
  }

  if (s_row_buf != NULL) {
    ESP_LOGI(TAG, "%s 重新初始化完成(深睡唤醒)", s_ctrl->name);
    return ESP_OK;
  }

  const size_t row_bytes = s_ctrl->width / 4;
  s_row_buf = heap_caps_malloc(row_bytes, MALLOC_CAP_DMA);
  if (s_row_buf == NULL) {
    ESP_LOGE(TAG, "行缓冲分配失败(%u 字节)", (unsigned)row_bytes);
    return ESP_ERR_NO_MEM;
  }

  ESP_LOGI(TAG, "%s ready: %ux%u,整帧 %u 字节", s_ctrl->name,
           (unsigned)s_ctrl->width, (unsigned)s_ctrl->height,
           (unsigned)display_frame_bytes());
  return ESP_OK;
}

esp_err_t display_init(void) {
  SemaphoreHandle_t lock = display_mutex();
  if (lock == NULL) {
    return ESP_ERR_NO_MEM;
  }
  xSemaphoreTake(lock, portMAX_DELAY);

  esp_err_t err = ESP_OK;
  if (!s_inited) {
    err = display_bringup();
    s_inited = (err == ESP_OK);
  }

  xSemaphoreGive(lock);
  return err;
}

esp_err_t display_show(display_read_t read_cb, void *ctx) {
  if (read_cb == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  // 允许 img 组件先于 periph probe 调用
  const esp_err_t init_err = display_init();
  if (init_err != ESP_OK) {
    return init_err;
  }

  SemaphoreHandle_t lock = display_mutex();
  if (lock == NULL) {
    return ESP_ERR_NO_MEM;
  }
  xSemaphoreTake(lock, portMAX_DELAY);
  display_pm_lock();
  s_bus_failed = false;

  esp_err_t err = ESP_OK;
  const size_t row_bytes = s_ctrl->width / 4;
  const int64_t t0 = esp_timer_get_time();

  for (uint16_t y = 0; y < s_ctrl->height; y++) {
    const int n = read_cb(ctx, s_row_buf, row_bytes);
    if (n != (int)row_bytes) {
      ESP_LOGE(TAG, "位图第 %u 行读取不足(%d/%u)", (unsigned)y, n,
               (unsigned)row_bytes);
      err = ESP_ERR_INVALID_SIZE;
      break;
    }
    s_ctrl->store_row(y, s_row_buf);
  }

  if (err == ESP_OK && s_bus_failed) {
    err = ESP_FAIL;
  }
  if (err == ESP_OK) {
    err = s_ctrl->flush();
    ESP_LOGI(TAG, "刷新%s,%u ms", err == ESP_OK ? "完成" : "失败",
             (unsigned)((esp_timer_get_time() - t0) / 1000));
  }
  // 超时也可能是屏已刷完但 BUSY 没释放：两种结果都进深睡，否则面板保持唤醒耗电；
  // 下次 show 由 display_init() 重新 bringup。
  s_ctrl->sleep();
  s_inited = false;

  display_pm_unlock();
  xSemaphoreGive(lock);
  return err;
}

// 按面板支持的颜色画竖彩条
typedef struct {
  uint8_t colors[4];
  uint8_t count;
  size_t pos;
} pattern_t;

static uint8_t pattern_colors(uint8_t *out) {
  if (s_ctrl == NULL) {
    return 0;
  }
  static const uint8_t order[] = {DISPLAY_COLOR_BLACK, DISPLAY_COLOR_WHITE,
                                  DISPLAY_COLOR_YELLOW, DISPLAY_COLOR_RED};
  uint8_t n = 0;
  for (size_t i = 0; i < sizeof(order); i++) {
    if (s_ctrl->color_mask & DISPLAY_COLOR_BIT(order[i])) {
      out[n++] = order[i];
    }
  }
  return n;
}

static int pattern_read(void *ctx, uint8_t *buf, size_t len) {
  pattern_t *p = (pattern_t *)ctx;
  const uint16_t width = s_ctrl->width;
  const size_t stride = width / 4;

  for (size_t i = 0; i < len; i++) {
    const size_t xb = (p->pos + i) % stride;
    uint8_t packed = 0;
    for (int k = 0; k < 4; k++) {
      const size_t x = xb * 4 + (size_t)k;
      uint8_t band = (uint8_t)(x * p->count / width);
      if (band >= p->count) {
        band = p->count - 1;
      }
      packed |= (uint8_t)(p->colors[band] & 0x03) << (6 - k * 2);
    }
    buf[i] = packed;
  }
  p->pos += len;
  return (int)len;
}

void display_self_test(void) {
  pattern_t p = {.count = 0, .pos = 0};
  p.count = pattern_colors(p.colors);
  if (p.count == 0) {
    ESP_LOGE(TAG, "面板未声明支持的颜色");
    return;
  }

  ESP_LOGI(TAG, "自检：%u 条等宽彩条(%s)", (unsigned)p.count, s_ctrl->name);
  const esp_err_t err = display_show(pattern_read, &p);
  ESP_LOGI(TAG, "自检 %s", err == ESP_OK ? "完成" : "失败");
}
