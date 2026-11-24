// 图片接收：控制帧 + 二进制分片 → FatFS 双槽轮换落盘 →(面板位图)刷到墨水屏。
// 协议见 img_recv.h。

#include "img_recv.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "cJSON.h"

#include "display.h"
#include "fatfs_store.h"

static const char *TAG = "img";

static img_send_text_t s_send;
static bool s_storage_mounted;

static FILE  *s_fp;
static char   s_name[IMG_NAME_MAX];        // 落盘占位名 image1/2.bin
static size_t s_total;
static size_t s_received;
static bool   s_is_panel;
static int    s_slot = -1;                 // 本次占用的槽位：0=image1.bin,1=image2.bin

static TaskHandle_t     s_show_task;
static QueueHandle_t    s_show_q;               // 待刷槽位(1 深)
static volatile int     s_reading_slot = -1;   // 刷屏任务正在读的槽位
static volatile int     s_queued_slot  = -1;   // 已排队待刷的槽位

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

    // 各回执 JSON 均以 '{' 开头,统一补 from:"esp" 便于上游日志识别来源。
    char out[224];
    if (body[0] == '{') {
        snprintf(out, sizeof(out), "{\"from\":\"esp\",%s", body + 1);
    } else {
        snprintf(out, sizeof(out), "%s", body);
    }
    s_send(out);
}

// 文件名固定,原名只进日志
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
    s_is_panel = false;
    s_slot = -1;
}

// 整屏刷新约 15s,与接收路径解耦

static int file_reader(void *ctx, uint8_t *buf, size_t len)
{
    return (int)fread(buf, 1, len, (FILE *)ctx);
}

static void show_task(void *arg)
{
    (void)arg;
    for (;;) {
        int slot;
        if (xQueueReceive(s_show_q, &slot, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        s_queued_slot = -1;
        s_reading_slot = slot;

        // 槽位即文件名,路径在这里推导,避免接收路径与刷屏路径共享缓冲
        char path[IMG_PATH_MAX];
        char name[IMG_NAME_MAX];
        image_path(slot, path, sizeof(path));
        snprintf(name, sizeof(name), "image%d.bin", slot + 1);

        FILE *fp = fopen(path, "rb");
        if (fp == NULL) {
            ESP_LOGE(TAG, "reopen %s failed", path);
            reply("{\"type\":\"img_error\",\"reason\":\"reopen failed\"}");
        } else {
            ESP_LOGI(TAG, "displaying %s on panel ...", name);
            esp_err_t err = display_show(file_reader, fp);
            fclose(fp);
            if (err == ESP_OK) {
                reply("{\"type\":\"img_shown\",\"name\":\"%s\"}", name);
            } else {
                reply("{\"type\":\"img_error\",\"reason\":\"panel refresh failed\"}");
            }
        }

        s_reading_slot = -1;
    }
}

/* 首次收文件时才挂载 FatFS：不用图片/音频的设备不占这份堆。 */
static esp_err_t img_recv_ensure(void)
{
    if (!s_storage_mounted) {
        esp_err_t err = fatfs_store_mount();
        if (err != ESP_OK) {
            return err;
        }
        s_storage_mounted = true;
    }
    return ESP_OK;
}

/* 只有真正要刷墨水屏时才建队列/刷屏任务；音频/raw 文件不需要 6KB 任务栈。 */
static esp_err_t img_recv_ensure_display(void)
{
    if (s_show_q == NULL) {
        s_show_q = xQueueCreate(1, sizeof(int));
        if (s_show_q == NULL) {
            ESP_LOGE(TAG, "show queue create failed");
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
        // 收满才排队刷屏,槽位占用到刷完；深度 1,接收路径已用 pick_slot 排他
        const int slot = s_slot;
        s_queued_slot = slot;
        if (s_show_q == NULL || xQueueSend(s_show_q, &slot, 0) != pdTRUE) {
            s_queued_slot = -1;
            ESP_LOGW(TAG, "show queue unavailable, skip refresh");
        }
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
        esp_err_t init_err = img_recv_ensure();
        if (init_err != ESP_OK) {
            ESP_LOGE(TAG, "storage init failed: %s", esp_err_to_name(init_err));
            reply("{\"type\":\"img_error\",\"reason\":\"storage init failed\"}");
            cJSON_Delete(root);
            return;
        }

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
        bool is_audio = cJSON_IsString(kind) && strcmp(kind->valuestring, "audio") == 0;

        // 未配 epd 执行器时 display 未初始化,先初始化才能取帧大小
        if (is_panel) {
            esp_err_t disp_err = img_recv_ensure_display();
            if (disp_err != ESP_OK) {
                ESP_LOGE(TAG, "display init failed: %s", esp_err_to_name(disp_err));
                reply("{\"type\":\"img_error\",\"reason\":\"display init failed\"}");
                cJSON_Delete(root);
                return;
            }
        }
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

        char path[IMG_PATH_MAX];
        if (is_audio) {
            // 音频固定落盘路径，不占用图片双槽
            snprintf(path, sizeof(path), FATFS_MOUNT_POINT "/audio.mp3");
            snprintf(s_name, sizeof(s_name), "audio.mp3");
            s_slot = -1;
        } else {
            // 避开正在刷的槽位
            int slot = pick_slot();
            if (slot < 0) {
                ESP_LOGW(TAG, "both slots busy (refresh in progress)");
                reply("{\"type\":\"img_error\",\"reason\":\"busy\"}");
                cJSON_Delete(root);
                return;
            }
            s_slot = slot;
            image_path(slot, path, sizeof(path));
            snprintf(s_name, sizeof(s_name), "image%d.bin", slot + 1);
        }

        ESP_LOGI(TAG, "img start: %s <- [%s] size=%u%s", path,
                 cJSON_IsString(name) ? name->valuestring : "?", (unsigned)total,
                 is_audio ? " (audio)" : is_panel ? " (panel bitmap)" : "");

        s_fp = fopen(path, "wb");
        if (s_fp == NULL) {
            ESP_LOGE(TAG, "open %s failed", path);
            reply("{\"type\":\"img_error\",\"reason\":\"open failed\"}");
            s_name[0] = '\0';
            cJSON_Delete(root);
            return;
        }

        s_total = total;
        s_received = 0;
        s_is_panel = is_panel;
        reply("{\"type\":\"img_ready\",\"name\":\"%s\",\"size\":%u,\"kind\":\"%s\"}",
              s_name, (unsigned)total,
              is_audio ? "audio" : is_panel ? "epd2bpp" : "raw");

    } else if (strcmp(type->valuestring, "img_end") == 0) {
        if (s_fp == NULL) {
            // 数据分片已自动收满并回执,忽略重复的 end。
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
    // 只记录回执回调；FatFS 与刷屏任务延迟到首次 img_start。
    s_send = send_text;
    return ESP_OK;
}

bool img_recv_handle(img_frame_type_t type, const uint8_t *data, int len)
{
    if (data == NULL || len <= 0) {
        return false;
    }

    if (type == IMG_FRAME_TEXT) {
        // '{' 开头才算控制帧,位图分片走二进制主题
        if (data[0] != '{') {
            return false;
        }
        on_text(data, len);
        return true;
    }

    on_binary(data, len);
    return true;
}
