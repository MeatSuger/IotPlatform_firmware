#include "sensor.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"

#define TAG "sensor"

/* --------------------------------------------------------------------------
 * Temperature collector — ESP32 内部温度传感器
 * -------------------------------------------------------------------------- */
#if SOC_TEMP_SENSOR_SUPPORTED

#include "driver/temperature_sensor.h"

static temperature_sensor_handle_t g_tempSensor = NULL;

static bool internal_temp_init(void)
{
    ESP_LOGI(TAG, "Install temperature sensor, range: -10~80 C");
    temperature_sensor_config_t cfg = {
        .range_min = -10,
        .range_max = 80,
        .clk_src = 0,
    };

    esp_err_t err = temperature_sensor_install(&cfg, &g_tempSensor);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "temperature_sensor_install failed: 0x%x", err);
        return false;
    }
    /* 不在 init 时 enable：采样时才上电（见 read_internal_temp），
     * 两次上报之间保持 disable，省 RTC 模拟域静态功耗 */
    ESP_LOGI(TAG, "Temperature sensor ready");
    return true;
}

static float read_internal_temp(void)
{
    float tsens_out = 0.0f;
    if (g_tempSensor == NULL) {
        ESP_LOGW(TAG, "Temperature sensor not initialized");
        return NAN; /* 故障哨兵，与真实 0°C 读数区分 */
    }
    /* 占空比采样：enable → 读数 → disable（失败路径同样关闭） */
    if (temperature_sensor_enable(g_tempSensor) != ESP_OK) {
        ESP_LOGE(TAG, "temperature_sensor_enable failed");
        return NAN;
    }
    esp_err_t err = temperature_sensor_get_celsius(g_tempSensor, &tsens_out);
    temperature_sensor_disable(g_tempSensor);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "get_celsius failed: 0x%x", err);
        return NAN; /* 故障路径不再返回 0.0f（与真实 0°C 混叠） */
    }
    return tsens_out;
}

/* temperature 采集器：value = 内部温度（两位小数）。def 暂不驱动行为 */
static cJSON *collect_temperature(const cJSON *def)
{
    (void)def;
    float raw = read_internal_temp();
    if (isnan(raw))
        return NULL; /* 本次采集失败：上报侧跳过该传感器 */
    double val = round((double)raw * 100.0) / 100.0;
    return cJSON_CreateNumber(val);
}

#else /* !SOC_TEMP_SENSOR_SUPPORTED */

static bool internal_temp_init(void)
{
    ESP_LOGW(TAG, "Temperature sensor not supported on this chip");
    return true; /* not fatal */
}

static cJSON *collect_temperature(const cJSON *def)
{
    (void)def;
    return NULL; /* 无内部温度传感器：该类型无可用采集器 */
}

#endif /* SOC_TEMP_SENSOR_SUPPORTED */

/* --------------------------------------------------------------------------
 * Collector registry — type → collect function.
 * -------------------------------------------------------------------------- */
typedef struct sensor_collector
{
    const char *type;
    sensor_collect_fn fn;
    struct sensor_collector *next;
} sensor_collector_t;

static sensor_collector_t *g_collectors = NULL;

bool sensor_register(const char *type, sensor_collect_fn fn)
{
    if (type == NULL || fn == NULL)
        return false;

    for (sensor_collector_t *c = g_collectors; c != NULL; c = c->next)
    {
        if (strcmp(c->type, type) == 0)
        {
            c->fn = fn; /* 覆盖注册 */
            return true;
        }
    }

    sensor_collector_t *c = calloc(1, sizeof *c);
    if (c == NULL)
        return false;
    c->type = type;
    c->fn = fn;
    c->next = g_collectors;
    g_collectors = c;
    return true;
}

static sensor_collect_fn collector_find(const char *type)
{
    for (sensor_collector_t *c = g_collectors; c != NULL; c = c->next)
    {
        if (strcmp(c->type, type) == 0)
            return c->fn;
    }
    return NULL;
}

bool sensor_init(void)
{
    bool ok = internal_temp_init();
    sensor_register("temperature", collect_temperature);
    return ok;
}

/* --------------------------------------------------------------------------
 * sensor_build_report — 按云端 sensors[] 定义采集并生成上报 JSON。
 * -------------------------------------------------------------------------- */
char *sensor_build_report(const char *config_json)
{
    if (config_json == NULL || config_json[0] == '\0')
        return NULL;

    cJSON *root = cJSON_Parse(config_json);
    if (root == NULL)
        return NULL;

    cJSON *defs = cJSON_GetObjectItem(root, "sensors");
    if (defs == NULL || !cJSON_IsArray(defs))
    {
        cJSON_Delete(root);
        return NULL;
    }

    cJSON *out = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    cJSON_AddItemToObject(out, "sensors", arr);

    const cJSON *def;
    cJSON_ArrayForEach(def, defs)
    {
        cJSON *id = cJSON_GetObjectItem(def, "id");
        cJSON *type = cJSON_GetObjectItem(def, "type");
        cJSON *enabled = cJSON_GetObjectItem(def, "enabled");
        if (!cJSON_IsString(id) || !cJSON_IsString(type))
            continue; /* 定义不完整 */
        if (cJSON_IsBool(enabled) && !cJSON_IsTrue(enabled))
            continue; /* disabled */

        sensor_collect_fn fn = collector_find(type->valuestring);
        if (fn == NULL)
        {
            ESP_LOGW(TAG, "sensor: 无 type='%s' 的采集器，跳过 (id=%s)",
                     type->valuestring, id->valuestring);
            continue;
        }

        cJSON *value = fn(def);
        if (value == NULL)
            continue; /* 本次采集失败 */

        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", id->valuestring);
        cJSON_AddStringToObject(item, "type", type->valuestring);
        cJSON_AddItemToObject(item, "value", value);
        cJSON_AddItemToArray(arr, item);
    }

    cJSON_Delete(root);

    if (cJSON_GetArraySize(arr) == 0)
    {
        cJSON_Delete(out);
        return NULL;
    }

    char *json = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    return json;
}
