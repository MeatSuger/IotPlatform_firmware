#ifndef _IMG_RECV_H_
#define _IMG_RECV_H_

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "fatfs_store.h" /* FATFS_MOUNT_POINT */

#ifdef __cplusplus
extern "C" {
#endif

/* img — 图片接收与刷屏（传输无关）：JSON 控制帧 → 分片落 FatFS 双槽轮换 → 独立任务流式刷屏。
 * kind="epd2bpp"（768×552 2bpp BWRY）落盘后刷屏，其余文件仅落盘。
 * 主题：iot/{id}/img/ctl 收 img_start/img_end；iot/{id}/img/data 收二进制分片；
 * 回执 iot/{id}/img/reply（img_ready/img_done/img_shown/img_error）。
 * 分两条主题：位图分片可能恰以 '{'(0x7B) 开头，按首字节判类型会误当控制帧整片丢弃。 */

typedef enum {
    IMG_FRAME_TEXT = 0,
    IMG_FRAME_BINARY = 1,
} img_frame_type_t;

#define IMG_MAX_SIZE (12 * 1024 * 1024) /* 单文件上限，< storage 分区 */
#define IMG_NAME_MAX 64
#define IMG_PATH_MAX 128
#define IMG_PANEL_SLOTS 2 /* 双槽轮换：刷屏读文件时不被新传输覆盖 */

typedef int (*img_send_text_t)(const char *text);

/* 启动：挂载 storage 分区到 /fatfs 并创建刷屏任务；重复调用安全。 */
esp_err_t img_recv_start(img_send_text_t send_text);

/* 处理一帧。返回 true = 已被图片接收逻辑消费。 */
bool img_recv_handle(img_frame_type_t type, const uint8_t *data, int len);

#ifdef __cplusplus
}
#endif

#endif /* _IMG_RECV_H_ */
