#include "appcfg.h"
#include "appcfg_logic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

#include "common.h"
#include "esp_log.h"
#include "mqtt_app.h"
#include "periph.h"

static const char *TAG = "appcfg";

static uint32_t s_appliedVersion = 0;
static char *s_payload = NULL;
static bool s_reportPending = false;
static bool s_inited = false;
static int s_reportMsgId = -1;

static SemaphoreHandle_t s_lock = NULL;

static void lock_init(void)
{
    if (s_lock == NULL)
        s_lock = xSemaphoreCreateMutex();
}

static void appcfg_lock(void)
{
    lock_init();
    if (s_lock != NULL)
        xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void appcfg_unlock(void)
{
    if (s_lock != NULL)
        xSemaphoreGive(s_lock);
}

static void nvs_save_pending(bool pending)
{
    nvs_handle_t h;
    if (nvs_open(APP_CFG_NVS_NS, NVS_READWRITE, &h) != ESP_OK)
        return;
    nvs_set_u8(h, APP_CFG_NVS_KEY_RPENDING, pending ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
}

/* QoS1 回执 PUBACK 到达后才清 pending */
static void appcfg_on_published(int msg_id)
{
    appcfg_lock();
    if (s_reportPending && msg_id == s_reportMsgId)
    {
        s_reportPending = false;
        s_reportMsgId = -1;
        nvs_save_pending(false);
        ESP_LOGI(TAG, "[appcfg] Config report delivered (version=%u)",
                      (unsigned)s_appliedVersion);
    }
    appcfg_unlock();
}

void appcfg_init(void)
{
    lock_init();
    if (s_inited)
        return;
    s_inited = true;

    mqtt_app_set_published_cb(appcfg_on_published);

    nvs_handle_t h;
    if (nvs_open(APP_CFG_NVS_NS, NVS_READONLY, &h) != ESP_OK)
    {
        ESP_LOGI(TAG, "[appcfg] No saved config (namespace empty)");
        return;
    }

    if (nvs_get_u32(h, APP_CFG_NVS_KEY_VERSION, &s_appliedVersion) != ESP_OK)
        s_appliedVersion = 0;

    size_t len = 0;
    if (nvs_get_str(h, APP_CFG_NVS_KEY_PAYLOAD, NULL, &len) == ESP_OK && len > 0)
    {
        char *buf = malloc(len);
        if (buf != NULL &&
            nvs_get_str(h, APP_CFG_NVS_KEY_PAYLOAD, buf, &len) == ESP_OK)
        {
            s_payload = buf;
        }
        else
        {
            free(buf);
        }
    }

    uint8_t p = 0;
    if (nvs_get_u8(h, APP_CFG_NVS_KEY_RPENDING, &p) == ESP_OK)
        s_reportPending = (p != 0);

    nvs_close(h);

    if (s_appliedVersion > 0)
        ESP_LOGI(TAG, "[appcfg] Restored applied cfg version=%u payload=%uB report_pending=%d",
                      (unsigned)s_appliedVersion,
                      s_payload != NULL ? (unsigned)strlen(s_payload) : 0,
                      s_reportPending);
}

/* 调用方负责 free */
char *appcfg_payload(void)
{
    appcfg_lock();
    char *copy = (s_payload != NULL) ? strdup(s_payload) : NULL;
    appcfg_unlock();
    return copy;
}

bool appcfg_report_pending(void)
{
    appcfg_lock();
    bool pending = s_reportPending;
    appcfg_unlock();
    return pending;
}

/* NVS 写成功后再更新内存；调用方持锁 */
static bool appcfg_persist(uint32_t version, const char *payload)
{
    if (payload == NULL || strlen(payload) > APP_CFG_PAYLOAD_MAX)
    {
        ESP_LOGE(TAG, "[appcfg] Invalid config payload (%u bytes)",
                      payload != NULL ? (unsigned)strlen(payload) : 0);
        return false;
    }

    char *copy = malloc(strlen(payload) + 1);
    if (copy == NULL)
        return false;
    strcpy(copy, payload);

    nvs_handle_t h;
    if (nvs_open(APP_CFG_NVS_NS, NVS_READWRITE, &h) != ESP_OK)
    {
        ESP_LOGE(TAG, "[appcfg] nvs_open failed");
        free(copy);
        return false;
    }

    esp_err_t err = nvs_set_u32(h, APP_CFG_NVS_KEY_VERSION, version);
    if (err == ESP_OK)
        err = nvs_set_str(h, APP_CFG_NVS_KEY_PAYLOAD, payload);
    if (err == ESP_OK)
        err = nvs_set_u8(h, APP_CFG_NVS_KEY_RPENDING, 1);
    if (err == ESP_OK)
        err = nvs_commit(h);
    nvs_close(h);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "[appcfg] NVS persist failed: 0x%x", err);
        free(copy);
        return false;
    }

    free(s_payload);
    s_payload = copy;
    s_appliedVersion = version;
    s_reportPending = true;
    return true;
}

/* reportInterval → g_reportIntervalMs,actuators → periph；
 * 返回 false 表示有执行器失败,不能回执 */
static bool apply_runtime_effects(const cJSON *config)
{
    if (config == NULL || !cJSON_IsObject(config))
        return true;

    const cJSON *sensor = cJSON_GetObjectItem(config, "sensor");
    const cJSON *ri = (sensor != NULL && cJSON_IsObject(sensor))
                          ? cJSON_GetObjectItem(sensor, "reportInterval")
                          : NULL;
    if (cJSON_IsNumber(ri) && ri->valuedouble >= 1.0 &&
        ri->valuedouble <= 86400.0)
    {
        g_reportIntervalMs = (uint32_t)(ri->valuedouble * 1000.0);
        ESP_LOGI(TAG, "[appcfg] report interval -> %u ms",
                      (unsigned)g_reportIntervalMs);
    }

    const cJSON *acts = cJSON_GetObjectItem(config, "actuators");
    if (acts != NULL && cJSON_IsArray(acts))
    {
        if (periph_apply_config(acts))
            ESP_LOGI(TAG, "[appcfg] actuators 已按期望列表收敛");
        else
        {
            ESP_LOGW(TAG, "[appcfg] actuators 部分条目应用失败(详见 periph 日志)");
            return false;
        }
    }
    return true;
}

/* 调用方持锁 */
static bool replay_payload(void)
{
    if (s_payload == NULL || s_payload[0] == '\0')
        return true;

    cJSON *cfg = cJSON_Parse(s_payload);
    if (cfg == NULL)
    {
        ESP_LOGE(TAG, "[appcfg] 持久化 payload 解析失败,跳过重放");
        return false;
    }
    bool ok = apply_runtime_effects(cfg);
    cJSON_Delete(cfg);
    return ok;
}

void appcfg_replay(void)
{
    if (!s_inited)
        appcfg_init();

    appcfg_lock();
    ESP_LOGI(TAG, "[appcfg] replay runtime effects (version=%u)",
                  (unsigned)s_appliedVersion);
    if (!replay_payload())
    {
        /* 开机重放失败必须补回执重试,否则本次开机永远不会再应用 */
        ESP_LOGW(TAG, "[appcfg] 开机重放执行器失败,标记待回执等待重试");
        if (!s_reportPending)
        {
            s_reportPending = true;
            nvs_save_pending(true);
        }
    }
    appcfg_unlock();
}

/* 调用方持锁；返回 malloc 的回执 JSON */
static char *build_report_locked(void)
{
    if (s_payload == NULL)
        return NULL;

    size_t need = strlen(s_payload) + 48;
    char *buf = malloc(need);
    if (buf == NULL)
        return NULL;
    snprintf(buf, need, "{\"version\":%u,\"config\":%s}",
             (unsigned)s_appliedVersion, s_payload);
    return buf;
}

/* QoS1 发布；PUBACK 到达后由 appcfg_on_published 清 pending */
static void send_report(void)
{
    appcfg_lock();
    if (!s_reportPending)
    {
        appcfg_unlock();
        return;
    }
    char *buf = build_report_locked();
    uint32_t version = s_appliedVersion;
    appcfg_unlock();

    if (buf == NULL)
        return;

    int msg_id = mqtt_publish_qos(MQTT_TOPIC_CONFIG_REPORT, buf, strlen(buf), 1);
    free(buf);

    if (msg_id < 0)
    {
        ESP_LOGW(TAG, "[appcfg] Config report publish failed (version=%u), will retry",
                      (unsigned)version);
        return;
    }

    appcfg_lock();
    if (s_appliedVersion == version && s_reportPending)
        s_reportMsgId = msg_id;
    appcfg_unlock();

    ESP_LOGI(TAG, "[appcfg] Config report queued (version=%u msg_id=%d)",
                  (unsigned)version, msg_id);
}

void appcfg_flush_pending_report(void)
{
    appcfg_lock();
    if (!s_reportPending)
    {
        appcfg_unlock();
        return;
    }
    /* 先重放再回执,避免把未生效的配置报成已应用 */
    bool replayed = replay_payload();
    appcfg_unlock();

    if (!replayed)
    {
        ESP_LOGW(TAG, "[appcfg] 待回执重放仍失败,继续等待下次重试");
        return;
    }
    send_report();
}

/* 调用方持锁；返回是否需要发回执 */
static bool handle_config_locked(uint32_t version, const cJSON *config)
{
    if (version < s_appliedVersion)
    {
        ESP_LOGI(TAG, "[appcfg] Ignore stale config version=%u (applied=%u)",
                      (unsigned)version, (unsigned)s_appliedVersion);
        return false;
    }

    if (!appcfg_should_apply(version, s_appliedVersion, s_reportPending,
                             s_payload != NULL))
    {
        ESP_LOGI(TAG, "[appcfg] Config version=%u already applied & acked",
                      (unsigned)version);
        return false;
    }

    /* 新版本,或同版本但本地 payload 丢失：重新持久化 */
    if (version > s_appliedVersion || s_payload == NULL)
    {
        char *cfgStr = cJSON_PrintUnformatted(config);
        if (cfgStr == NULL)
            return false;
        bool saved = appcfg_persist(version, cfgStr);
        free(cfgStr);
        if (!saved)
            return false;
        ESP_LOGI(TAG, "[appcfg] Applied config version=%u", (unsigned)version);
    }

    /* 运行时未生效则保持 rpending,等周期任务重试 */
    if (!replay_payload())
    {
        ESP_LOGE(TAG, "[appcfg] 配置已保存但运行时生效失败,保持待回执并重试 (version=%u)",
                      (unsigned)version);
        return false;
    }
    return true;
}

void appcfg_handle_config(uint32_t version, const cJSON *config)
{
    if (config == NULL || !cJSON_IsObject(config))
    {
        ESP_LOGE(TAG, "[appcfg] Invalid config envelope (no config object)");
        return;
    }

    appcfg_lock();
    bool should_report = handle_config_locked(version, config);
    appcfg_unlock();

    if (should_report)
        send_report();
}
