// 图片接收：控制帧 + 二进制分片 → FatFS 双槽轮换落盘 →(面板位图)刷到墨水屏。
// 协议见 img_recv.h。

#include "img_recv.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "cJSON.h"

#include "display.h"
#include "fatfs_store.h"

static const char *TAG = "img";

static img_send_text_t s_send;
static bool s_storage_mounted;

static FILE  *s_fp;
static char   s_name[IMG_NAME_MAX];        // 落盘占位名 image1/2.bin
static char   s_cur_path[IMG_PATH_MAX];
static size_t s_total;
static size_t s_received;
static bool   s_is_panel;
static int    s_slot = -1;                 // 本次占用的槽位：0=image1.bin，1=image2.bin

static TaskHandle_t     s_show_task;
static SemaphoreHandle_t s_show_sem;
static volatile int     s_reading_slot = -1;   // 刷屏任务正在读的槽位
static volatile int     s_queued_slot  = -1;   // 已排队待刷的槽位
static char             s_show_path[IMG_PATH_MAX];
static char             s_show_name[IMG_NAME_MAX];

static void reply(const char *fmt, ...)
{
    if (s_send == NULL) {
        return;
    }
    char body[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);

    // 各回执 JSON 均以 '{' 开头，统一补 from:"esp" 便于上游日志识别来源。
    char out[224];
    if (body[0] == '{') {
        snprintf(out, sizeof(out), "{\"from\":\"esp\",%s", body + 1);
    } else {
        snprintf(out, sizeof(out), "%s", body);
    }
    s_send(out);
}

// 文件名固定，原名只进日志
static void image_path(int slot, char *out, size_t outsz)
{
    snprintf(out, outsz, FATFS_MOUNT_POINT "/image%d.bin", slot + 1);
}

static void transfer_reset(void)
{
    if (s_fp) {
        fclose(s_fp);
        s_fp = NULL;
    }
    s_total = 0;
    s_received = 0;
    s_name[0] = '\0';
    s_cur_path[0] = '\0';
    s_is_panel = false;
    s_slot = -1;
}

// 整屏刷新约 15s，与接收路径解耦

static int file_reader(void *ctx, uint8_t *buf, size_t len)
{
    return (int)fread(buf, 1, len, (FILE *)ctx);
}

static void show_task(void *arg)
{
    (void)arg;
    while (true) {
        if (xSemaphoreTake(s_show_sem, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        int slot = s_queued_slot;
        s_queued_slot = -1;
        s_reading_slot = slot;

        FILE *fp = fopen(s_show_path, "rb");
        if (fp == NULL) {
            ESP_LOGE(TAG, "reopen %s failed", s_show_path);
            reply("{\"type\":\"img_error\",\"reason\":\"reopen failed\"}");
        } else {
            ESP_LOGI(TAG, "displaying %s on panel ...", s_show_name);
            esp_err_t err = display_show(file_reader, fp);
            fclose(fp);
            if (err == ESP_OK) {
                reply("{\"type\":\"img_shown\",\"name\":\"%s\"}", s_show_name);
            } else {
                reply("{\"type\":\"img_error\",\"reason\":\"panel refresh failed\"}");
            }
        }

        s_reading_slot = -1;
    }
}

// 两个槽位都在用时返回 -1
static int pick_slot(void)
{
    for (int s = 0; s < IMG_PANEL_SLOTS; s++) {
        if (s != s_reading_slot && s != s_queued_slot) {
            return s;
        }
    }
    return -1;
}

static void finish_ok(void)
{
    fclose(s_fp);
    s_fp = NULL;
    ESP_LOGI(TAG, "img done: %s (%u bytes)", s_name, (unsigned)s_received);
    reply("{\"type\":\"img_done\",\"name\":\"%s\",\"bytes\":%u}", s_name, (unsigned)s_received);

    if (s_is_panel && s_slot >= 0) {
        // 收满才排队刷屏，槽位占用到刷完
        strncpy(s_show_path, s_cur_path, sizeof(s_show_path) - 1);
        s_show_path[sizeof(s_show_path) - 1] = '\0';
        strncpy(s_show_name, s_name, sizeof(s_show_name) - 1);
        s_show_name[sizeof(s_show_name) - 1] = '\0';
        s_queued_slot = s_slot;
        xSemaphoreGive(s_show_sem);
    }

    s_is_panel = false;
    s_slot = -1;
}

// img_start：校验大小与面板尺寸

static void on_text(const uint8_t *data, int len)
{
    cJSON *root = cJSON_ParseWithLength((const char *)data, len);
    if (root == NULL) {
        ESP_LOGW(TAG, "bad control json");
        return;
    }

    const cJSON *type = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(type)) {
        cJSON_Delete(root);
        return;
    }

    if (strcmp(type->valuestring, "img_start") == 0) {
        const cJSON *name = cJSON_GetObjectItem(root, "name");
        const cJSON *size = cJSON_GetObjectItem(root, "size");
        const cJSON *kind = cJSON_GetObjectItem(root, "kind");

        transfer_reset();

        size_t total = cJSON_IsNumber(size) ? (size_t)size->valuedouble : 0;
        if (total == 0 || total > IMG_MAX_SIZE) {
            ESP_LOGE(TAG, "bad size: %u", (unsigned)total);
            reply("{\"type\":\"img_error\",\"reason\":\"bad size\"}");
            cJSON_Delete(root);
            return;
        }

        bool is_panel = cJSON_IsString(kind) && strcmp(kind->valuestring, "epd2bpp") == 0;

        // 未配 epd 执行器时 display 未初始化，先初始化才能取帧大小
        if (is_panel && display_init() != ESP_OK) {
            ESP_LOGE(TAG, "panel init failed");
            reply("{\"type\":\"img_error\",\"reason\":\"panel init failed\"}");
            cJSON_Delete(root);
            return;
        }

        if (is_panel && total != display_frame_bytes()) {
            ESP_LOGE(TAG, "panel bitmap size mismatch: %u != %u", (unsigned)total,
                     (unsigned)display_frame_bytes());
            reply("{\"type\":\"img_error\",\"reason\":\"panel size mismatch\"}");
            cJSON_Delete(root);
            return;
        }

        // 避开正在刷的槽位
        int slot = pick_slot();
        if (slot < 0) {
            ESP_LOGW(TAG, "both slots busy (refresh in progress)");
            reply("{\"type\":\"img_error\",\"reason\":\"busy\"}");
            cJSON_Delete(root);
            return;
        }

        s_slot = slot;
        image_path(slot, s_cur_path, sizeof(s_cur_path));
        snprintf(s_name, sizeof(s_name), "image%d.bin", slot + 1);

        ESP_LOGI(TAG, "img start: %s <- [%s] size=%u%s", s_cur_path,
                 cJSON_IsString(name) ? name->valuestring : "?", (unsigned)total,
                 is_panel ? " (panel bitmap)" : "");

        s_fp = fopen(s_cur_path, "wb");
        if (s_fp == NULL) {
            ESP_LOGE(TAG, "open %s failed", s_cur_path);
            reply("{\"type\":\"img_error\",\"reason\":\"open failed\"}");
            s_name[0] = '\0';
            cJSON_Delete(root);
            return;
        }

        s_total = total;
        s_received = 0;
        s_is_panel = is_panel;
        reply("{\"type\":\"img_ready\",\"name\":\"%s\",\"size\":%u,\"kind\":\"%s\"}",
              s_name, (unsigned)total, is_panel ? "epd2bpp" : "raw");

    } else if (strcmp(type->valuestring, "img_end") == 0) {
        if (s_fp == NULL) {
            // 数据分片已自动收满并回执，忽略重复的 end。
        } else if (s_received == s_total) {
            finish_ok();
        } else {
            ESP_LOGE(TAG, "img incomplete: %u/%u", (unsigned)s_received, (unsigned)s_total);
            reply("{\"type\":\"img_error\",\"reason\":\"incomplete\"}");
            transfer_reset();
        }
    } else {
        ESP_LOGI(TAG, "unknown text control: %s", type->valuestring);
    }

    cJSON_Delete(root);
}

static void on_binary(const uint8_t *data, int len)
{
    if (s_fp == NULL) {
        ESP_LOGW(TAG, "binary without img_start, drop %d bytes", len);
        return;
    }

    size_t wr = fwrite(data, 1, len, s_fp);
    if (wr != (size_t)len) {
        ESP_LOGE(TAG, "fwrite short: %u/%d", (unsigned)wr, len);
        reply("{\"type\":\"img_error\",\"reason\":\"write failed\"}");
        transfer_reset();
        return;
    }

    s_received += len;
    if (s_received >= s_total) {
        finish_ok();
    }
}

esp_err_t img_recv_start(img_send_text_t send_text)
{
    s_send = send_text;

    // 只挂一次，失败可重试
    if (!s_storage_mounted) {
        esp_err_t err = fatfs_store_mount();
        if (err != ESP_OK) {
            return err;
        }
        s_storage_mounted = true;
    }

    if (s_show_sem == NULL) {
        s_show_sem = xSemaphoreCreateBinary();
        if (s_show_sem == NULL) {
            ESP_LOGE(TAG, "semaphore create failed");
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_show_task == NULL) {
        // 栈 6KB：只读文件并流式喂驱动
        if (xTaskCreate(show_task, "img_show", 6 * 1024, NULL, 4, &s_show_task) != pdPASS) {
            ESP_LOGE(TAG, "show task create failed");
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

bool img_recv_handle(img_frame_type_t type, const uint8_t *data, int len)
{
    if (data == NULL || len <= 0) {
        return false;
    }

    if (type == IMG_FRAME_TEXT) {
        // '{' 开头才算控制帧，位图分片走二进制主题
        if (data[0] != '{') {
            return false;
        }
        on_text(data, len);
        return true;
    }

    on_binary(data, len);
    return true;
}
