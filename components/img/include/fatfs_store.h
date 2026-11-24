#ifndef _FATFS_STORE_H_
#define _FATFS_STORE_H_

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FATFS_MOUNT_POINT "/fatfs"
#define FATFS_PARTITION_LABEL "storage"

/* 挂载 storage 分区到 FATFS_MOUNT_POINT；重复调用安全 */
esp_err_t fatfs_store_mount(void);

#ifdef __cplusplus
}
#endif

#endif /* _FATFS_STORE_H_ */
