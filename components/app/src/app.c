#include "app.h"
#include "periph.h"
#include "mqtt_app.h"
#include "appcfg.h"
#include "sensor.h"

#include "cJSON.h"

/* --------------------------------------------------------------------------
 * sensorReportTask — FreeRTOS task on Core 0: 周期上报传感器遥测。
 *
 * 数据源 = 云端物模型（config.sensors[] 定义，见 appcfg_payload）：
 * 按每条定义的 type 找采集器读取（读类设备），组装
 *   {"sensors":[{"name":<id>,"type":<type>,"value":...}]}
 * 发布到 iot/{id}/telemetry。无定义/全部采集失败 → 跳过本轮（仅状态切换时告警一次，
 * 避免每周期刷日志）。
 * -------------------------------------------------------------------------- */
void sensorReportTask(void *pvParameters)
{
    DEBUG_PRINT("[Core %d] Sensor report task started\n",
                (int)xPortGetCoreID());

    bool lastReportOk = true; /* 首次无定义时打印一次 */

    for (;;)
    {
        EventBits_t bits = xEventGroupGetBits(g_wifiEventGroup);
        if (bits & WIFI_CONNECTED_BIT)
        {
            /* 配置回执兜底：应用成功但回执未发出时周期性补发 */
            if (appcfg_report_pending())
                appcfg_flush_pending_report();

            /* 按云端传感器定义采集上报（物模型为准） */
            const char *cfg = appcfg_payload();
            char *report = (cfg != NULL) ? sensor_build_report(cfg) : NULL;
            if (report != NULL)
            {
                if (!mqtt_publish(MQTT_TOPIC_DATA, report, strlen(report)))
                    DEBUG_PRINTLN("[Core 0] Sensor MQTT publish failed");
                free(report);
                lastReportOk = true;
            }
            else if (lastReportOk)
            {
                DEBUG_PRINTLN("[Core 0] 无可用传感器定义/采集失败，等待云端配置 sensors 后开始上报");
                lastReportOk = false;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(g_reportIntervalMs));
    }
}

/* --------------------------------------------------------------------------
 * cmdProcessTask — FreeRTOS task on Core 1: 出队并分发后端消息。
 *
 * 执行器定义唯一真源为云端配置快照（appcfg → periph_apply_config）；
 * 设备类型（transport: gpio/pwm/spi/...）由定义 config.transport 决定，
 * 命令 value 为传输原语（云端负责角度/颜色等语义换算）。本任务只做分发：
 *   1. 配置快照（version+config）→ appcfg_handle_config（幂等/持久化/回执）
 *   2. 控制命令（action = 执行器 id → periph_dispatch）
 * -------------------------------------------------------------------------- */
void cmdProcessTask(void *pvParameters)
{
    DEBUG_PRINT("[Core %d] Command processing task started\n",
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
            DEBUG_PRINTLN("[cmd] 消息 JSON 解析失败，丢弃");
            continue;
        }

        /* --- 入站信封归一化 ---
         * 三种来源形态：
         *   A. 配置快照（MQTT retained 主题 iot/{id}/config 原文）:
         *        {"version":N,"config":{...}}
         *   B. 后端下行命令（MQTT iot/{id}/cmd 实时 / HTTP GET /commands 兜底）:
         *        {"id":N,"type":"config"|"control","payload":{...},"createdAt":...}
         * A/B 均解出“内容子对象”后分发（version+config → 配置状态机；
         * action → 执行器 transport 路由），命令无应答（控制为 fire-and-forget，
         * 最终状态经遥测上报体现）。 */
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
                DEBUG_PRINT("[cmd] 控制命令未执行: action=%s\n",
                            actionItem->valuestring);
        }
        else
        {
            DEBUG_PRINTLN("[cmd] 未知消息信封，忽略");
        }

        cJSON_Delete(root);
    }
}
