#include "sensor.h"
#include <cJSON.h>

/**
 * createSensorData - Build the sensor JSON document
 * Return: compact JSON string, caller owns the returned String
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
	cJSON_AddNumberToObject(internalTemp, "value",
				roundf(temperatureRead() * 100) / 100.0f);

	char *jsonStr = cJSON_PrintUnformatted(root);
	String jsonString = String(jsonStr);
	cJSON_free(jsonStr);
	cJSON_Delete(root);
	return jsonString;
}
