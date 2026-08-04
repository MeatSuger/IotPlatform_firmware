#include "task.h"
#include "sensor.h"
#include "network.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"

/* --------------------------------------------------------------------------
 * LEDC configuration for analogWrite (PWM) replacement.
 *
 * The original code uses analogWrite(pin, duty) with duty in [0, 255].
 * ESP32 has no true DAC on most pins; analogWrite is backed by LEDC PWM.
 *
 * We allocate LEDC channels on-the-fly per GPIO pin, mimicking Arduino.
 * -------------------------------------------------------------------------- */
#define LEDC_MODE LEDC_LOW_SPEED_MODE
#define LEDC_RESOLUTION LEDC_TIMER_13_BIT
#define LEDC_FREQ_HZ 5000

typedef struct
{
    int gpio;
    ledc_channel_t channel;
    bool in_use;
} ledc_slot_t;

static ledc_slot_t ledc_slots[8];
static bool ledc_initialized = false;

static void ledc_init(void)
{
    if (ledc_initialized)
        return;

    for (int i = 0; i < 8; i++)
    {
        ledc_slots[i].gpio = -1;
        ledc_slots[i].channel = i;
        ledc_slots[i].in_use = false;
    }

    ledc_timer_config_t timer_cfg = {
        .speed_mode = LEDC_MODE,
        .duty_resolution = LEDC_RESOLUTION,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer_cfg));
    ledc_initialized = true;
}

static ledc_channel_t ledc_acquire_channel(int gpio)
{
    ledc_init();

    for (int i = 0; i < 8; i++)
    {
        if (ledc_slots[i].in_use && ledc_slots[i].gpio == gpio)
            return ledc_slots[i].channel;
    }

    for (int i = 0; i < 8; i++)
    {
        if (!ledc_slots[i].in_use)
        {
            ledc_slots[i].gpio = gpio;
            ledc_slots[i].in_use = true;

            ledc_channel_config_t ch_cfg = {
                .gpio_num = gpio,
                .speed_mode = LEDC_MODE,
                .channel = ledc_slots[i].channel,
                .timer_sel = LEDC_TIMER_0,
                .duty = 0,
                .hpoint = 0,
            };
            ESP_ERROR_CHECK(ledc_channel_config(&ch_cfg));
            return ledc_slots[i].channel;
        }
    }

    DEBUG_PRINTLN("WARNING: all LEDC channels in use, reusing ch0");
    return LEDC_CHANNEL_0;
}

static void analog_write(int pin, int duty)
{
    if (duty < 0)
        duty = 0;
    if (duty > 255)
        duty = 255;

    ledc_channel_t ch = ledc_acquire_channel(pin);
    uint32_t scaled = (uint32_t)duty * 8191 / 255;
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_MODE, ch, scaled));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_MODE, ch));
}

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
 * cmdProcessTask — Dequeue, execute, and respond to commands on Core 1.
 *
 * Supported payload.action values:
 *   "on"     — gpio_set_level(pin, 1)
 *   "off"    — gpio_set_level(pin, 0)
 *   "toggle" — gpio_set_level(pin, !gpio_get_level(pin))
 *   "pwm"    — analog_write(pin, duty), value range 0–100
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

            if (gpio_json != NULL && cJSON_IsString(gpio_json) &&
                action_json != NULL && cJSON_IsString(action_json))
            {

                int pin = atoi(gpio_json->valuestring);
                const char *action = action_json->valuestring;

                // 对于 on/off/toggle，先复位为 GPIO 模式
                if (strcmp(action, "on") == 0 || strcmp(action, "off") == 0 || strcmp(action, "toggle") == 0)
                {
                    gpio_reset_pin(pin); // 清除复用功能
                    gpio_set_direction(pin, GPIO_MODE_INPUT_OUTPUT);
                }

                if (strcmp(action, "on") == 0)
                {
                    gpio_set_level(pin, 1);
                    executed = true;
                }
                else if (strcmp(action, "off") == 0)
                {
                    gpio_set_level(pin, 0);
                    executed = true;
                }
                else if (strcmp(action, "toggle") == 0)
                {
                    int cur = gpio_get_level(pin);
                    DEBUG_PRINT("Toggling GPIO %d: current level=%d\n", pin, cur);
                    gpio_set_level(pin, !cur);
                    executed = true;
                }
                else if (strcmp(action, "pwm") == 0)
                {
                    if (value_json != NULL && cJSON_IsNumber(value_json))
                    {
                        int raw = value_json->valueint;
                        raw = constrain_int(raw, 0, 100);
                        int duty = (raw * 255 + 50) / 100;
                        analog_write(pin, duty);
                        executed = true;
                    }
                }
                else
                {
                    DEBUG_PRINT("Unknown action: %s\n", action);
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
