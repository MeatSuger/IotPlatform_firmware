#include "app.h"
#include "network.h"
#include "periph.h"
#include "ledc_pool.h"
#include "mqtt_app.h"

#include "driver/gpio.h"
#include "cJSON.h"
#include "sensor.h"

static int constrain_int(int val, int min, int max)
{
    if (val < min)
        return min;
    if (val > max)
        return max;
    return val;
}

/* --------------------------------------------------------------------------
 * sensorReportTask — FreeRTOS task: periodically publish sensor data
 * via MQTT on Core 0.
 * -------------------------------------------------------------------------- */
void sensorReportTask(void *pvParameters)
{
    DEBUG_PRINT("[Core %d] Sensor report task started\n",
                (int)xPortGetCoreID());

    for (;;)
    {
        EventBits_t bits = xEventGroupGetBits(g_wifiEventGroup);
        if (bits & WIFI_CONNECTED_BIT)
        {
            if (!sensor_publish_data())
                DEBUG_PRINTLN("[Core 0] Sensor MQTT publish failed");
        }
        vTaskDelay(pdMS_TO_TICKS(SEND_INTERVAL));
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

        /* Protocol format (strict):
         *   {"deviceId":"90431b","type":"register","payload":{...}}
         *                              "type":"control"
         * type "register" — device registration actions (config/unconfig)
         * type "control"  — device control / legacy GPIO commands
         * deviceId is mandatory and must match this device. */
        cJSON *deviceId = cJSON_GetObjectItem(root, "deviceId");
        cJSON *type = cJSON_GetObjectItem(root, "type");
        bool is_register = cJSON_IsString(type) &&
                           strcmp(type->valuestring, "register") == 0;
        bool is_control = cJSON_IsString(type) &&
                          strcmp(type->valuestring, "control") == 0;
        if (!cJSON_IsString(deviceId) ||
            strcmp(deviceId->valuestring, DEVICE_ID) != 0 ||
            (!is_register && !is_control))
        {
            DEBUG_PRINTLN("Invalid command envelope (deviceId=%s, type=%s)",
                          cJSON_IsString(deviceId)
                              ? deviceId->valuestring
                              : "(null)",
                          cJSON_IsString(type)
                              ? type->valuestring
                              : "(null)");
            cJSON_Delete(root);
            continue;
        }

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

                if (is_register)
                {
                    /* Registration type: only config / unconfig. */
                    if (strcmp(action, "config") == 0 ||
                        strcmp(action, "unconfig") == 0)
                    {
                        executed = periph_dispatch(pl);
                    }
                    else
                    {
                        DEBUG_PRINT("Register command with invalid "
                                    "action: %s\n",
                                    action);
                    }
                }
                else if (periph_device_find(action) != NULL)
                {
                    /* Control type: route to a registered device. */
                    executed = periph_dispatch(pl);
                }
                else if (gpio_json != NULL && cJSON_IsString(gpio_json))
                {
                    /* Legacy GPIO actions (on/off/toggle/pwm). */
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
        cJSON_AddStringToObject(resp, "status",
                                executed ? "ok" : "skipped");

        char *respStr = cJSON_PrintUnformatted(resp);
        mqtt_publish_response(respStr);
        free(respStr);
        cJSON_Delete(resp);
        cJSON_Delete(root);
    }
}
