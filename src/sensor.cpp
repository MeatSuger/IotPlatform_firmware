#include "sensor.h"
#include <cJSON.h>

/**
 * createSensorData - Build the sensor JSON document.
 *
 * Temperature is rounded to two decimal places and serialised as a
 * pre-formatted string (e.g. "25.45") so that the JSON output never
 * leaks floating-point artefacts like 25.450000000000001.
 *
 * Return: compact JSON string, caller owns the returned String.
 */
String createSensorData(void)
{
	cJSON *root = cJSON_CreateObject();
	cJSON *sensors = cJSON_CreateArray();
	cJSON_AddItemToObject(root, "sensors", sensors);

	cJSON *internalTemp = cJSON_CreateObject();
	cJSON_AddItemToArray(sensors, internalTemp);
	cJSON_AddStringToObject(internalTemp, "name", "tempuatre");
	cJSON_AddStringToObject(internalTemp, "type", "temp");

	char buf[16];
	snprintf(buf, sizeof(buf), "%.2f", temperatureRead());
	cJSON_AddStringToObject(internalTemp, "value", buf);

	char *jsonStr = cJSON_PrintUnformatted(root);
	String jsonString = String(jsonStr);
	cJSON_free(jsonStr);
	cJSON_Delete(root);
	return jsonString;
}
