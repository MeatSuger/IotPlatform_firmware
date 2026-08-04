#include "sensor.h"

#include <math.h>
#include "cJSON.h"
#include "esp_log.h"
#include "sdkconfig.h"

/* --------------------------------------------------------------------------
 * Temperature sensor: use the new driver on chips that support it
 * (ESP32-S2/S3/C2/C3/C5/C6/H2/P4), fall back to ADC on ESP32 classic.
 * -------------------------------------------------------------------------- */
#if SOC_TEMP_SENSOR_SUPPORTED

#include "driver/temperature_sensor.h"

static temperature_sensor_handle_t g_tempSensor = NULL;

bool sensor_init(void)
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
    err = temperature_sensor_enable(g_tempSensor);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "temperature_sensor_enable failed: 0x%x", err);
        return false;
    }
    ESP_LOGI(TAG, "Temperature sensor ready");
    return true;
}

static float read_internal_temp(void)
{
    float tsens_out = 0.0f;
    if (g_tempSensor == NULL) {
        ESP_LOGW(TAG, "Temperature sensor not initialized");
        return 0.0f;
    }
    esp_err_t err = temperature_sensor_get_celsius(g_tempSensor, &tsens_out);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "get_celsius failed: 0x%x", err);
    }
    return tsens_out;
}

#else  /* !SOC_TEMP_SENSOR_SUPPORTED — ESP32 classic: no usable driver */

bool sensor_init(void)
{
    ESP_LOGW(TAG, "Temperature sensor not supported on this chip");
    return true;  /* not fatal */
}

static float read_internal_temp(void)
{
    return 0.0f;  /* no sensor available */
}

#endif /* SOC_TEMP_SENSOR_SUPPORTED */

/* --------------------------------------------------------------------------
 * createSensorData — Build the sensor JSON document.
 *
 * Returns: malloc'd compact JSON string, caller must free.
 * -------------------------------------------------------------------------- */
char *createSensorData(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *sensors = cJSON_CreateArray();
    cJSON_AddItemToObject(root, "sensors", sensors);

    cJSON *internalTemp = cJSON_CreateObject();
    cJSON_AddItemToArray(sensors, internalTemp);
    cJSON_AddStringToObject(internalTemp, "name", "temperature");
    cJSON_AddStringToObject(internalTemp, "type", "temp");

    double val = round(read_internal_temp() * 100.0) / 100.0;
    cJSON_AddNumberToObject(internalTemp, "value", val);

    char *jsonStr = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return jsonStr;
}
