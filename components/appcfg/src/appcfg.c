#include "appcfg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nvs_flash.h"

#include "common.h"
#include "mqtt_app.h"
#include "periph.h"

#define NVS_NAMESPACE      "appcfg"
#define NVS_KEY_VERSION    "version"
#define NVS_KEY_PAYLOAD    "payload"
#define NVS_KEY_RPENDING   "rpending"

/* 配置 payload 单条上限（NVS 整条读写，云端配置快照通常 < 1KB） */
#define CONFIG_PAYLOAD_MAX 4095U

static uint32_t s_appliedVersion = 0;
static char *s_payload = NULL;
static bool s_reportPending = false;
static bool s_inited = false;

/* --------------------------------------------------------------------------
 * NVS 持久化
 * -------------------------------------------------------------------------- */

static void nvs_save_pending(bool pending)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK)
        return;
    nvs_set_u8(h, NVS_KEY_RPENDING, pending ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
}

void appcfg_init(void)
{
    if (s_inited)
        return;
    s_inited = true;

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK)
    {
        DEBUG_PRINTLN("[appcfg] No saved config (namespace empty)");
        return;
    }

    if (nvs_get_u32(h, NVS_KEY_VERSION, &s_appliedVersion) != ESP_OK)
        s_appliedVersion = 0;

    size_t len = 0;
    if (nvs_get_str(h, NVS_KEY_PAYLOAD, NULL, &len) == ESP_OK && len > 0)
    {
        char *buf = malloc(len);
        if (buf != NULL &&
            nvs_get_str(h, NVS_KEY_PAYLOAD, buf, &len) == ESP_OK)
        {
            s_payload = buf;
        }
        else
        {
            free(buf);
        }
    }

    uint8_t p = 0;
    if (nvs_get_u8(h, NVS_KEY_RPENDING, &p) == ESP_OK)
        s_reportPending = (p != 0);

    nvs_close(h);

    if (s_appliedVersion > 0)
        DEBUG_PRINTLN("[appcfg] Restored applied cfg version=%u payload=%uB report_pending=%d",
                      (unsigned)s_appliedVersion,
                      s_payload != NULL ? (unsigned)strlen(s_payload) : 0,
                      s_reportPending);
}

uint32_t appcfg_version(void)
{
    return s_appliedVersion;
}

const char *appcfg_payload(void)
{
    return s_payload;
}

bool appcfg_report_pending(void)
{
    return s_reportPending;
}

/* 持久化成功后才更新内存态（崩溃安全：NVS 先行） */
static bool appcfg_persist(uint32_t version, const char *payload)
{
    if (payload == NULL || strlen(payload) > CONFIG_PAYLOAD_MAX)
    {
        DEBUG_PRINTLN("[appcfg] Invalid config payload (%u bytes)",
                      payload != NULL ? (unsigned)strlen(payload) : 0);
        return false;
    }

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK)
    {
        DEBUG_PRINTLN("[appcfg] nvs_open failed");
        return false;
    }

    esp_err_t err = nvs_set_u32(h, NVS_KEY_VERSION, version);
    if (err == ESP_OK)
        err = nvs_set_str(h, NVS_KEY_PAYLOAD, payload);
    if (err == ESP_OK)
        err = nvs_set_u8(h, NVS_KEY_RPENDING, 1);
    if (err == ESP_OK)
        err = nvs_commit(h);
    nvs_close(h);

    if (err != ESP_OK)
    {
        DEBUG_PRINTLN("[appcfg] NVS persist failed: 0x%x", err);
        return false;
    }

    free(s_payload);
    s_payload = malloc(strlen(payload) + 1);
    if (s_payload == NULL)
        return false;
    strcpy(s_payload, payload);
    s_appliedVersion = version;
    s_reportPending = true;
    return true;
}

void appcfg_report_sent(void)
{
    s_reportPending = false;
    nvs_save_pending(false);
}

/* --------------------------------------------------------------------------
 * 配置分区运行时效果应用（payload 分区语义见 appcfg.h）
 *
 * 当前硬件能力映射（其余字段不生效但随 payload 原样持久化/回执）：
 *   - sensor.reportInterval（秒）→ g_reportIntervalMs（传感器上报周期）
 *   - actuators[] → periph_apply_config()（执行器 diff 实例化/卸载）
 * -------------------------------------------------------------------------- */
static void apply_runtime_effects(const cJSON *config)
{
    if (config == NULL || !cJSON_IsObject(config))
        return;

    const cJSON *sensor = cJSON_GetObjectItem(config, "sensor");
    const cJSON *ri = (sensor != NULL && cJSON_IsObject(sensor))
                          ? cJSON_GetObjectItem(sensor, "reportInterval")
                          : NULL;
    if (cJSON_IsNumber(ri) && ri->valuedouble >= 1.0 &&
        ri->valuedouble <= 86400.0)
    {
        g_reportIntervalMs = (uint32_t)(ri->valuedouble * 1000.0);
        DEBUG_PRINTLN("[appcfg] report interval -> %u ms",
                      (unsigned)g_reportIntervalMs);
    }

    const cJSON *acts = cJSON_GetObjectItem(config, "actuators");
    if (acts != NULL && cJSON_IsArray(acts))
    {
        if (periph_apply_config(acts))
            DEBUG_PRINTLN("[appcfg] actuators 已按期望列表收敛");
        else
            DEBUG_PRINTLN("[appcfg] actuators 部分条目应用失败（详见 periph 日志）");
    }
}

/* 重放已持久化 payload 的运行时效果（开机恢复 / 新版本应用共用） */
static void replay_payload(void)
{
    if (s_payload == NULL || s_payload[0] == '\0')
        return;

    cJSON *cfg = cJSON_Parse(s_payload);
    if (cfg == NULL)
    {
        DEBUG_PRINTLN("[appcfg] 持久化 payload 解析失败，跳过重放");
        return;
    }
    apply_runtime_effects(cfg);
    cJSON_Delete(cfg);
}

void appcfg_replay(void)
{
    if (!s_inited)
        appcfg_init();
    DEBUG_PRINTLN("[appcfg] replay runtime effects (version=%u)",
                  (unsigned)s_appliedVersion);
    replay_payload();
}

/* 用内存中已应用 payload 原文发布回执（成功才清除待回执标志） */
static void send_report(void)
{
    if (s_payload == NULL)
        return;

    size_t need = strlen(s_payload) + 48;
    char *buf = malloc(need);
    if (buf == NULL)
        return;
    snprintf(buf, need, "{\"version\":%u,\"config\":%s}",
             (unsigned)s_appliedVersion, s_payload);

    bool ok = mqtt_publish(MQTT_TOPIC_CONFIG_REPORT, buf, strlen(buf));
    free(buf);

    if (ok)
    {
        DEBUG_PRINTLN("[appcfg] Config report sent (version=%u)",
                      (unsigned)s_appliedVersion);
        appcfg_report_sent();
    }
    else
    {
        DEBUG_PRINTLN("[appcfg] Config report publish failed (version=%u), will retry",
                      (unsigned)s_appliedVersion);
    }
}

void appcfg_flush_pending_report(void)
{
    if (s_reportPending)
        send_report();
}

void appcfg_handle_config(uint32_t version, const cJSON *config)
{
    if (config == NULL || !cJSON_IsObject(config))
    {
        DEBUG_PRINTLN("[appcfg] Invalid config envelope (no config object)");
        return;
    }

    if (version < s_appliedVersion)
    {
        DEBUG_PRINTLN("[appcfg] Ignore stale config version=%u (applied=%u)",
                      (unsigned)version, (unsigned)s_appliedVersion);
        return;
    }

    if (version == s_appliedVersion && !s_reportPending)
    {
        /* retained 重投的幂等分支：已应用且已回执，无需动作 */
        DEBUG_PRINTLN("[appcfg] Config version=%u already applied & acked",
                      (unsigned)version);
        return;
    }

    if (version > s_appliedVersion)
    {
        char *cfgStr = cJSON_PrintUnformatted(config);
        if (cfgStr == NULL)
            return;
        if (!appcfg_persist(version, cfgStr))
        {
            free(cfgStr);
            return;
        }
        free(cfgStr);
        DEBUG_PRINTLN("[appcfg] Applied config version=%u", (unsigned)version);
        /* 从持久化 payload 统一重放运行时效果（与开机 appcfg_replay 同路径） */
        replay_payload();
    }

    /* version == applied 且待回执：保留重投 → 直接补发回执 */
    send_report();
}
