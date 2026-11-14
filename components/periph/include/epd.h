#ifndef _EPD_H_
#define _EPD_H_

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* epd — JD79665（华为 A1）3.98" 768x552 四色 BWRY 墨水屏，板载固定外设；
 * 同时注册 "epd" transport，云端可用 actuators 定义它参与引脚仲裁。
 * 接线（SPI2 IOMUX 原生脚）：MOSI=11 SCK=12 CS=10 DC=9 RST=8 BUSY=7（低=忙）。
 * 位图：2bpp BWRY，4 像素/字节，MSB first，行优先；颜色 0=黑 1=白 2=黄 3=红；
 * 字节地址 y*(768/4)+x/4，像素 x+k 落在 bit(6-2k)。
 */

#define EPD_BITMAP_W     768
#define EPD_BITMAP_H     552
#define EPD_BITMAP_SIZE  ((EPD_BITMAP_W * EPD_BITMAP_H) / 4)   /* 105984 字节 */

/* 位图数据源：把最多 len 字节读入 buf，返回实际读取字节数；<=0 视为失败 */
typedef int (*epd_bitmap_read_t)(void *ctx, uint8_t *buf, size_t len);

/* 把一张 EPD_BITMAP_SIZE 字节的位图流式写入面板并整屏刷新（约 15 s）。
 * 内部按 16KB 分块取数据；阻塞调用，只应在专用刷屏任务里执行。 */
esp_err_t epd_show_bitmap_stream(epd_bitmap_read_t read_cb, void *ctx);

/* 自检：画 4 条彩条（黑/白/黄/红），与传图走同一条通路 */
void epd_app(void);

#ifdef __cplusplus
}
#endif

#endif /* _EPD_H_ */
