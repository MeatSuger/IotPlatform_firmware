#ifndef _DISPLAY_CTRL_H_
#define _DISPLAY_CTRL_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 控制器适配层接口：display.c 负责调度，适配层只做命令序列与数据组织。 */

typedef struct display_ctrl {
  const char *name;
  uint16_t width;
  uint16_t height;
  uint8_t color_mask;           /* 支持的颜色位图：决定自检画几条彩条 */
  bool busy_high_means_busy;    /* BUSY 极性：true=高电平为忙（SSD1683），false=低（JD79665） */
  esp_err_t (*init)(void);                                     /* 复位 + 寄存器 */
  void (*store_row)(uint16_t y, const uint8_t *logical_row);    /* 逻辑行 → 原生缓冲 */
  esp_err_t (*flush)(void);                                    /* 送屏并刷新 */
  void (*sleep)(void);
} display_ctrl_t;

/* 具体控制器（新增面板实现同签名实例并在 display.c 登记） */
extern const display_ctrl_t display_ctrl_ssd1683_4in2;
extern const display_ctrl_t display_ctrl_jd79665_3in98;

#ifdef __cplusplus
}
#endif

#endif /* _DISPLAY_CTRL_H_ */
