// 图片存储分区挂载：label="storage" 的 FAT 分区 → /fatfs，带 wear_levelling。

#include "fatfs_store.h"

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "wear_levelling.h"

static const char *TAG = "fatfs";

esp_err_t fatfs_store_mount(void)
{
    const esp_vfs_fat_mount_config_t cfg = {
        .format_if_mount_failed = true,
        .max_files = 4,
        .allocation_unit_size = 8 * 1024,
    };

    wl_handle_t wl = WL_INVALID_HANDLE;
    esp_err_t ret = esp_vfs_fat_spiflash_mount_rw_wl(FATFS_MOUNT_POINT, FATFS_PARTITION_LABEL, &cfg, &wl);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "mount %s failed: %s", FATFS_MOUNT_POINT, esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "mounted %s (partition \"%s\")", FATFS_MOUNT_POINT, FATFS_PARTITION_LABEL);
    return ESP_OK;
}
