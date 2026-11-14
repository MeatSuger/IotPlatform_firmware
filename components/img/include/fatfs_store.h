#ifndef _FATFS_STORE_H_
#define _FATFS_STORE_H_

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FATFS_MOUNT_POINT "/fatfs"
#define FATFS_PARTITION_LABEL "storage"

/* 挂载分区表中 label=FATFS_PARTITION_LABEL 的 FAT 分区（带磨损均衡）到
 * FATFS_MOUNT_POINT；幂等由调用方保证。 */
esp_err_t fatfs_store_mount(void);

#ifdef __cplusplus
}
#endif

#endif /* _FATFS_STORE_H_ */
