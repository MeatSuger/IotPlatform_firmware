#ifndef _IMG_RECV_H_
#define _IMG_RECV_H_

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "fatfs_store.h" /* FATFS_MOUNT_POINT */

#ifdef __cplusplus
extern "C" {
#endif

/* 图片接收：JSON 控制帧 → 分片落 FatFS 双槽 → 独立任务刷屏。
 * kind="epd2bpp" 刷 2bpp 面板位图，其余文件只落盘。
 * 主题：img/ctl、img/data、img/reply。 */

typedef enum {
    IMG_FRAME_TEXT = 0,
    IMG_FRAME_BINARY = 1,
} img_frame_type_t;

#define IMG_MAX_SIZE (12 * 1024 * 1024) /* 单文件上限，< storage 分区 */
#define IMG_NAME_MAX 64
#define IMG_PATH_MAX 128
#define IMG_PANEL_SLOTS 2 /* 双槽轮换，刷屏时不覆盖 */

typedef int (*img_send_text_t)(const char *text);

/* 启动：挂载 storage 分区到 /fatfs 并创建刷屏任务；重复调用安全。 */
esp_err_t img_recv_start(img_send_text_t send_text);

/* 处理一帧。返回 true = 已被图片接收逻辑消费。 */
bool img_recv_handle(img_frame_type_t type, const uint8_t *data, int len);

#ifdef __cplusplus
}
#endif

#endif /* _IMG_RECV_H_ */
