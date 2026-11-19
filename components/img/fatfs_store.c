// 图片存储分区挂载：label="storage" 的 FAT 分区 → /fatfs,带 wear_levelling。

#include "fatfs_store.h"

#include "esp_log.h"
#include "esp_partition.h"
#include "esp_vfs_fat.h"
#include "wear_levelling.h"

static const char *TAG = "fatfs";

/* 只有整块仍是擦除态(0xFF)才允许格式化；坏 FAT 不自动清盘 */
static bool partition_is_blank(void)
{
    const esp_partition_t *p = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT,
        FATFS_PARTITION_LABEL);
    if (p == NULL)
        return false;

    uint8_t probe[64];
    if (esp_partition_read(p, 0, probe, sizeof probe) != ESP_OK)
        return false;

    for (size_t i = 0; i < sizeof probe; i++)
        if (probe[i] != 0xFF)
            return false;
    return true;
}

esp_err_t fatfs_store_mount(void)
{
    const bool blank = partition_is_blank();
    const esp_vfs_fat_mount_config_t cfg = {
        .format_if_mount_failed = blank,
        .max_files = 4,
        .allocation_unit_size = 8 * 1024,
    };
    if (blank)
        ESP_LOGW(TAG, "storage partition is blank, formatting");

    wl_handle_t wl = WL_INVALID_HANDLE;
    esp_err_t ret = esp_vfs_fat_spiflash_mount_rw_wl(FATFS_MOUNT_POINT, FATFS_PARTITION_LABEL, &cfg, &wl);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "mount %s failed: %s (format %s)", FATFS_MOUNT_POINT,
                 esp_err_to_name(ret),
                 blank ? "attempted" : "skipped, partition not blank");
        return ret;
    }

    ESP_LOGI(TAG, "mounted %s (partition \"%s\")", FATFS_MOUNT_POINT, FATFS_PARTITION_LABEL);
    return ESP_OK;
}
