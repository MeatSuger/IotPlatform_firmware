#include "task.h"
#include "network.h"
#include "periph.h"
#include "ledc_pool.h"

#include "driver/gpio.h"
#include "cJSON.h"

static int constrain_int(int val, int min, int max)
{
    if (val < min)
        return min;
    if (val > max)
        return max;
    return val;
}

/* --------------------------------------------------------------------------
 * httpUploadTask — FreeRTOS task: periodically POST sensor data on Core 0.
 * -------------------------------------------------------------------------- */
void httpUploadTask(void *pvParameters)
{
    DEBUG_PRINT("[Core %d] HTTP upload task started\n",
                (int)xPortGetCoreID());

    for (;;)
    {
        EventBits_t bits = xEventGroupGetBits(g_wifiEventGroup);
        if (bits & WIFI_CONNECTED_BIT)
        {
            if (!sendSensorData())
                DEBUG_PRINTLN("[Core 0] HTTP upload failed");
        }
        vTaskDelay(pdMS_TO_TICKS(SEND_INTERVAL));
    }
}

/* --------------------------------------------------------------------------
 * mqtt_publish_response — helper to publish a JSON response via MQTT.
 * -------------------------------------------------------------------------- */
static void mqtt_publish_response(const char *jsonStr)
{
    /* Snapshot the shared handle to avoid use-after-free if the main
     * task destroys the MQTT client concurrently (TOCTOU). */
    esp_mqtt_client_handle_t client = g_mqttClient;

    if (!g_isMQTTConnected || client == NULL)
    {
        DEBUG_PRINT("MQTT disconnected, response dropped: %s\n", jsonStr);
        return;
    }

    int msg_id = esp_mqtt_client_publish(client,
                                         MQTT_TOPIC_RESP,
                                         jsonStr, strlen(jsonStr),
                                         0, 0);
    if (msg_id < 0)
    {
        DEBUG_PRINTLN("MQTT publish failed");
    }
    else
    {
        DEBUG_PRINT("Response sent [msg_id=%d]: %s\n", msg_id, jsonStr);
    }
}

/* --------------------------------------------------------------------------
 * handle_gpio_action — legacy GPIO actions (on/off/toggle/pwm) for
 * backwards compatibility with the pre-peripheral-bus command protocol.
 * New peripherals (led/servo/speaker/...) are handled via periph_dispatch.
 * -------------------------------------------------------------------------- */
static bool handle_gpio_action(int pin, const char *action,
                               cJSON *value_json)
{
    /* 对于 on/off/toggle，先复位为 GPIO 模式（清除复用功能） */
    if (strcmp(action, "on") == 0 || strcmp(action, "off") == 0 ||
        strcmp(action, "toggle") == 0)
    {
        gpio_reset_pin(pin);
        gpio_set_direction(pin, GPIO_MODE_INPUT_OUTPUT);
    }

    if (strcmp(action, "on") == 0)
    {
        gpio_set_level(pin, 1);
        return true;
    }
    if (strcmp(action, "off") == 0)
    {
        gpio_set_level(pin, 0);
        return true;
    }
    if (strcmp(action, "toggle") == 0)
    {
        int cur = gpio_get_level(pin);
        DEBUG_PRINT("Toggling GPIO %d: current level=%d\n", pin, cur);
        gpio_set_level(pin, !cur);
        return true;
    }
    if (strcmp(action, "pwm") == 0)
    {
        if (value_json != NULL && cJSON_IsNumber(value_json))
        {
            int raw = constrain_int(value_json->valueint, 0, 100);
            int duty = (raw * 255 + 50) / 100;
            return ledc_pool_set_duty_pct(pin, duty);
        }
        return false;
    }

    DEBUG_PRINT("Unknown action: %s\n", action);
    return false;
}

/* --------------------------------------------------------------------------
 * cmdProcessTask — Dequeue, execute, and respond to commands on Core 1.
 *
 * Command routing:
 *   1. Bus-level actions and registered devices go to the peripheral bus
 *      (periph_dispatch): "config", "unconfig", or any action matching a
 *      configured device name (e.g. "led", "servo", "speaker").
 *   2. Legacy GPIO actions ("on"/"off"/"toggle"/"pwm" + "GPIO" field).
 *
 * Publishes a JSON response via MQTT after each command.
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
            mqtt_publish_response(
                "{\"type\":\"response\",\"status\":\"parse_error\"}");
            continue;
        }

        cJSON *id = cJSON_GetObjectItem(root, "id");
        cJSON *pl = cJSON_GetObjectItem(root, "payload");
        bool executed = false;

        if (pl != NULL)
        {
            cJSON *gpio_json = cJSON_GetObjectItem(pl, "GPIO");
            cJSON *action_json = cJSON_GetObjectItem(pl, "action");
            cJSON *value_json = cJSON_GetObjectItem(pl, "value");

            if (action_json != NULL && cJSON_IsString(action_json))
            {
                const char *action = action_json->valuestring;

                if (strcmp(action, "config") == 0 ||
                    strcmp(action, "unconfig") == 0 ||
                    periph_device_find(action) != NULL)
                {
                    executed = periph_dispatch(pl);
                }
                else if (gpio_json != NULL && cJSON_IsString(gpio_json))
                {
                    executed = handle_gpio_action(atoi(gpio_json->valuestring),
                                                  action, value_json);
                }
                else
                {
                    DEBUG_PRINT("Unknown action or missing GPIO field: %s\n",
                                action);
                }
            }
        }

        /* Build response */
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddStringToObject(resp, "type", "response");
        if (id != NULL)
            cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(id, 1));
        cJSON_AddStringToObject(resp, "status",
                                executed ? "ok" : "skipped");

        char *respStr = cJSON_PrintUnformatted(resp);
        mqtt_publish_response(respStr);
        free(respStr);
        cJSON_Delete(resp);
        cJSON_Delete(root);
    }
}
