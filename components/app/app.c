#include "app.h"
#include "common.h"
#include "periph.h"
#include "mqtt_app.h"
#include "appcfg.h"
#include "sensor.h"

#include <stdlib.h>
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "app";

/* Core 0 周期上报遥测；无定义或采集失败时跳过本轮 */

void sensorReportTask(void *pvParameters)
{
    (void)pvParameters;
    ESP_LOGI(TAG, "[Core %d] Sensor report task started",
                (int)xPortGetCoreID());

    bool lastReportOk = true; /* 只告警一次 */

    for (;;)
    {
        EventBits_t bits = xEventGroupGetBits(g_wifiEventGroup);
        /* MQTT 未连接时跳过，避免启动时序刷错误日志 */
        if ((bits & WIFI_CONNECTED_BIT) && g_isMQTTConnected)
        {
            /* 周期补发待回执 */
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

/* Core 1 出队分发：配置走 appcfg，控制命令走 periph */

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

        /* 兼容配置快照 {version,config} 与后端信封 {id,type,payload} */
        cJSON *sub = root;
        cJSON *envPayload = cJSON_GetObjectItem(root, "payload");
        if (envPayload != NULL && cJSON_IsObject(envPayload))
            sub = envPayload;

        cJSON *verItem = cJSON_GetObjectItem(sub, "version");
        cJSON *cfgItem = cJSON_GetObjectItem(sub, "config");
        if (cJSON_IsNumber(verItem) && cJSON_IsObject(cfgItem))
        {
            /* 版本幂等、NVS、执行器 diff、回执都在 appcfg 内 */
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
