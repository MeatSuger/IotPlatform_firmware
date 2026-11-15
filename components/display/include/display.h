#ifndef _DISPLAY_H_
#define _DISPLAY_H_

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 显示层（面板无关）。逻辑位图（前端按此打包）：2bpp BWRY，4 像素/字节，
 * MSB first，行优先；0=黑 1=白 2=黄 3=红。 */

/* 板载固定接线（SPI2 IOMUX 原生脚） */
#define DISPLAY_PIN_MOSI 11
#define DISPLAY_PIN_SCK 12
#define DISPLAY_PIN_CS 10
#define DISPLAY_PIN_DC 9
#define DISPLAY_PIN_RST 8
#define DISPLAY_PIN_BUSY 7

#define DISPLAY_SPI_HZ (10 * 1000 * 1000)

/* 逻辑颜色编号 */
enum {
  DISPLAY_COLOR_BLACK = 0,
  DISPLAY_COLOR_WHITE = 1,
  DISPLAY_COLOR_YELLOW = 2,
  DISPLAY_COLOR_RED = 3,
};
#define DISPLAY_COLOR_BIT(c) (1u << (c))

typedef struct {
  uint16_t width;
  uint16_t height;
  const char *ctrl; /* 控制器名，仅用于日志 */
} display_info_t;

/* 数据源：读入最多 len 字节，返回实际字节数；<=0 视为失败 */
typedef int (*display_read_t)(void *ctx, uint8_t *buf, size_t len);

/* 当前面板信息与整帧字节数（width*height/4） */
const display_info_t *display_info(void);
size_t display_frame_bytes(void);

/* 探测到的控制器 id（如 "SSD1683"）；未探测到返回 NULL */
const char *display_chip_id(void);

/* 面板色深 "bw"/"bwr"/"bwry"；未探测到返回 NULL */
const char *display_color_depth(void);

/* 初始化显示（总线 + 引脚 + 控制器）；重复调用安全 */
esp_err_t display_init(void);

/* 流式写入整屏位图并刷新（阻塞）；成功后自动进深睡，下次调用会重新初始化 */
esp_err_t display_show(display_read_t read_cb, void *ctx);

/* 自检：按面板支持的颜色画等宽竖彩条 */
void display_self_test(void);

/* 总线原语（供适配层调用，适配层不直接碰 SPI/GPIO） */
void display_bus_cmd(uint8_t cmd);
void display_bus_data(uint8_t data);
void display_bus_write(const void *buf, size_t len);
/* 等 BUSY 释放；busy_high=真 即「高=忙」；返回 false 表示超时 */
bool display_bus_wait_busy(bool busy_high, uint32_t timeout_ms,
                           const char *tag);

#ifdef __cplusplus
}
#endif

#endif /* _DISPLAY_H_ */
