#include "task.h"
#include "sensor.h"
#include "network.h"

/**
 * httpUploadTask - FreeRTOS task: periodically POST sensor data on Core 0
 * @pvParameters: unused
 */
void httpUploadTask(void *pvParameters)
{
	DEBUG_PRINT("[Core 0] HTTP upload task started, running on core: ");
	DEBUG_PRINTLN(xPortGetCoreID());

	for (;;) {
		if (WiFi.status() == WL_CONNECTED &&
		    !authorizationToken.isEmpty()) {
			if (!sendSensorData())
				DEBUG_PRINTLN("[Core 0] HTTP upload failed");
		}
		vTaskDelay(pdMS_TO_TICKS(SEND_INTERVAL));
	}
}

/**
 * cmdProcessTask - FreeRTOS task: dequeue, execute, and respond to WS commands
 * @pvParameters: unused
 *
 * Supported payload.action values:
 *   "on"     - digitalWrite(pin, HIGH)
 *   "off"    - digitalWrite(pin, LOW)
 *   "toggle" - digitalWrite(pin, !digitalRead(pin))
 *   "pwm"    - analogWrite(pin, duty) with value 0-100
 *
 * Sends a JSON response back via WebSocket after each command.
 */
void cmdProcessTask(void *pvParameters)
{
	DEBUG_PRINTF("[Core %d] Command processing task started\n",
		     xPortGetCoreID());
	CommandMsg cmdMsg;

	for (;;) {
		if (xQueueReceive(commandQueue, &cmdMsg, portMAX_DELAY) !=
		    pdTRUE)
			continue;

		cJSON *root = cJSON_Parse(cmdMsg.payload);
		if (root == NULL) {
			webSocket.sendTXT(
				"{\"type\":\"response\",\"status\":\"parse_error\"}");
			continue;
		}

		cJSON *id = cJSON_GetObjectItem(root, "id");
		cJSON *pl = cJSON_GetObjectItem(root, "payload");
		bool executed = false;

		if (pl != NULL) {
			cJSON *gpio = cJSON_GetObjectItem(pl, "GPIO");
			cJSON *action = cJSON_GetObjectItem(pl, "action");
			cJSON *value = cJSON_GetObjectItem(pl, "value");
			if (gpio != NULL && cJSON_IsString(gpio) &&
			    action != NULL && cJSON_IsString(action)) {
				int pin = atoi(gpio->valuestring);
				pinMode(pin, OUTPUT);
				switch (action->valuestring[0]) {
				case 'o':
					if (action->valuestring[1] == 'n')
						digitalWrite(pin, HIGH);
					else
						digitalWrite(pin, LOW);
					executed = true;
					break;
				case 't':
					digitalWrite(pin, !digitalRead(pin));
					executed = true;
					break;
				case 'p':
					if (value != NULL &&
					    cJSON_IsNumber(value)) {
						int raw = value->valueint;
						raw = constrain(raw, 0, 100);
						int duty = (raw * 255 + 50) /
							   100;
						analogWrite(pin, duty);
						executed = true;
					}
					break;
				default:
					DEBUG_PRINTF("Unknown action: %s\n",
						     action->valuestring);
					break;
				}
			}
		}

		/* Build and send WebSocket response */
		cJSON *resp = cJSON_CreateObject();
		cJSON_AddStringToObject(resp, "type", "response");
		if (id != NULL)
			cJSON_AddItemToObject(resp, "id",
					     cJSON_Duplicate(id, 1));
		cJSON_AddStringToObject(resp, "status",
					executed ? "ok" : "skipped");
		char *respStr = cJSON_PrintUnformatted(resp);
		webSocket.sendTXT(respStr);
		DEBUG_PRINTF("Response sent: %s\n", respStr);
		cJSON_free(respStr);
		cJSON_Delete(resp);

		cJSON_Delete(root);
	}
}
