#include "app.h"
#include "common.h"
#include "periph.h"
#include "mqtt_app.h"
#include "appcfg.h"
#include "sensor.h"

#include <stdlib.h> /* malloc/free */
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "app";

/* --------------------------------------------------------------------------
 * sensorReportTask — Core 0 周期上报传感器遥测。
 *
 * 数据源 = 云端物模型（config.sensors[] 定义，见 appcfg_payload），
 * 组装 {"sensors":[...]} 发布到 iot/{id}/telemetry；无定义/全部采集失败时
 * 跳过本轮，仅状态切换时告警一次（避免每周期刷日志）。
 * -------------------------------------------------------------------------- */
void sensorReportTask(void *pvParameters)
{
    (void)pvParameters;
    ESP_LOGI(TAG, "[Core %d] Sensor report task started",
                (int)xPortGetCoreID());

    bool lastReportOk = true; /* 保证首次无定义时只打印一次 */

    for (;;)
    {
        EventBits_t bits = xEventGroupGetBits(g_wifiEventGroup);
        /* MQTT 未连接时跳过本轮：避免启动时序（WiFi 已连、MQTT 尚未 connected）
         * 产生虚假的 "Sensor MQTT publish failed" 错误日志。 */
        if ((bits & WIFI_CONNECTED_BIT) && g_isMQTTConnected)
        {
            /* 配置回执兜底：应用成功但回执未发出时周期性补发 */
            if (appcfg_report_pending())
                appcfg_flush_pending_report();

            const char *cfg = appcfg_payload();
            char *report = (cfg != NULL) ? sensor_build_report(cfg) : NULL;
            if (report != NULL)
            {
                if (!mqtt_publish(MQTT_TOPIC_DATA, report, strlen(report)))
                    ESP_LOGE(TAG, "[Core 0] Sensor MQTT publish failed");
                free(report);
                lastReportOk = true;
            }
            else if (lastReportOk)
            {
                ESP_LOGI(TAG, "[Core 0] 无可用传感器定义/采集失败，等待云端配置 sensors 后开始上报");
                lastReportOk = false;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(g_reportIntervalMs));
    }
}

/* --------------------------------------------------------------------------
 * cmdProcessTask — Core 1 出队并分发后端消息。
 *
 * 执行器定义唯一真源为云端配置快照（appcfg → periph_apply_config）；
 * 设备类型（transport）由定义 specs.transport 决定，命令 value 为传输原语
 * （云端负责角度/颜色等语义换算）。本任务只做分发：
 *   1. 配置快照（version+config）→ appcfg_handle_config
 *   2. 控制命令（action = 执行器 id）→ periph_dispatch
 * -------------------------------------------------------------------------- */
void cmdProcessTask(void *pvParameters)
{
    (void)pvParameters;
    ESP_LOGI(TAG, "[Core %d] Command processing task started",
                (int)xPortGetCoreID());
    CommandMsg cmdMsg;

    for (;;)
    {
        if (xQueueReceive(g_commandQueue, &cmdMsg, portMAX_DELAY) != pdTRUE)
            continue;

        cJSON *root = cJSON_Parse(cmdMsg.payload);
        free(cmdMsg.payload);
        cmdMsg.payload = NULL;

        if (root == NULL)
        {
            ESP_LOGE(TAG, "[cmd] 消息 JSON 解析失败，丢弃");
            continue;
        }

        /* --- 入站信封归一化 ---
         * 三种来源形态：
         *   A. 配置快照（MQTT retained 主题 iot/{id}/config 原文）:
         *        {"version":N,"config":{...}}
         *   B. 后端下行命令（MQTT iot/{id}/cmd 实时 / HTTP GET /commands 兜底）:
         *        {"id":N,"type":"config"|"control","payload":{...},"createdAt":...}
         * 解出“内容子对象”后分发（version+config → 配置状态机；action → 执行器路由）。 */
        cJSON *sub = root;
        cJSON *envPayload = cJSON_GetObjectItem(root, "payload");
        if (envPayload != NULL && cJSON_IsObject(envPayload))
            sub = envPayload;

        cJSON *verItem = cJSON_GetObjectItem(sub, "version");
        cJSON *cfgItem = cJSON_GetObjectItem(sub, "config");
        if (cJSON_IsNumber(verItem) && cJSON_IsObject(cfgItem))
        {
            /* 配置快照：版本幂等 + NVS + 执行器 diff + 回执，见 appcfg */
            appcfg_handle_config((uint32_t)verItem->valuedouble, cfgItem);
            cJSON_Delete(root);
            continue;
        }

        cJSON *actionItem = cJSON_GetObjectItem(sub, "action");
        if (cJSON_IsString(actionItem))
        {
            if (!periph_dispatch(sub))
                ESP_LOGW(TAG, "[cmd] 控制命令未执行: action=%s",
                            actionItem->valuestring);
        }
        else
        {
            ESP_LOGW(TAG, "[cmd] 未知消息信封，忽略");
        }

        cJSON_Delete(root);
    }
}
