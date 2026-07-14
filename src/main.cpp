#include "main.hpp"

/* Global state — exposed via extern in main.hpp */
String authorizationToken = "";
WebSocketsClient webSocket;
QueueHandle_t commandQueue = NULL;
bool isWSConnected = false;

void setup(void)
{
#if DEBUG_ENABLE
	Serial.begin(115200);
#endif
	DEBUG_PRINTLN("Device starting...");

	if (!connectToWiFi()) {
		DEBUG_PRINTLN("WiFi connection failed, restarting device...");
		ESP.restart();
		return;
	}

	if (!authbydeviceid()) {
		DEBUG_PRINTLN("Authentication failed, restarting device...");
		ESP.restart();
		return;
	}

	commandQueue = xQueueCreate(10, sizeof(CommandMsg));
	if (commandQueue == NULL) {
		DEBUG_PRINTLN("Failed to create command queue");
		ESP.restart();
	}

	webSocket.beginSSL(WSS_HOST, 443, WSS_URL);
	webSocket.setExtraHeaders("Origin=https://" API_HOST);
	webSocket.setAuthorization(authorizationToken.c_str());
	webSocket.setReconnectInterval(5000);
	webSocket.onEvent(webSocketEvent);

	/* Core 0: periodic HTTP upload */
	xTaskCreatePinnedToCore(httpUploadTask, "HttpUploadTask", 10240, NULL,
				1, NULL, 0);

	/* Core 1: command processing (offloads JSON parsing from ISR) */
	xTaskCreatePinnedToCore(cmdProcessTask, "CmdProcessTask", 8192, NULL, 2,
				NULL, 1);

	DEBUG_PRINTLN("System init complete");
}

void loop(void)
{
	if (WiFi.status() != WL_CONNECTED) {
		DEBUG_PRINTLN("WiFi disconnected, attempting to reconnect...");
		isWSConnected = false;
		if (!connectToWiFi() || !authbydeviceid()) {
			DEBUG_PRINTLN("Reconnection failed, restarting...");
			ESP.restart();
			return;
		}
		webSocket.setAuthorization(authorizationToken.c_str());
	}

	webSocket.loop();
	delay(50);
}

/**
 * webSocketEvent - WebSocket library callback
 * @type:    event type (connected, disconnected, text, error, ping, pong)
 * @payload: raw frame data (valid only during this callback)
 * @length:  payload byte count
 *
 * On WStype_TEXT, enqueues a CommandMsg for asynchronous processing.
 */
void webSocketEvent(WStype_t type, uint8_t *payload, size_t length)
{
	switch (type) {
	case WStype_DISCONNECTED:
		DEBUG_PRINTLN("WebSocket disconnected");
		isWSConnected = false;
		break;
	case WStype_CONNECTED:
		DEBUG_PRINTLN("WebSocket connected");
		isWSConnected = true;
		webSocket.sendTXT("Hello Server!");
		break;
	case WStype_TEXT: {
		DEBUG_PRINTF("Message received: %s\n", (char *)payload);
		CommandMsg cmdMsg;
		cmdMsg.payload = (char *)payload;
		cmdMsg.length = length;
		if (xQueueSend(commandQueue, &cmdMsg, 0) != pdPASS)
			DEBUG_PRINTLN("Command queue full, dropping message");
		break;
	}
	case WStype_ERROR:
		DEBUG_PRINTF("WebSocket error: %s\n", (char *)payload);
		break;
	case WStype_PING:
		DEBUG_PRINTLN("Ping received");
		break;
	case WStype_PONG:
		DEBUG_PRINTLN("Pong received");
		break;
	default:
		DEBUG_PRINTF("Unknown event type: %s\n", (char *)payload);
		break;
	}
}

/**
 * connectToWiFi - Block until connected or 15 s timeout expires
 * Return: true on success, false on timeout
 */
bool connectToWiFi(void)
{
	DEBUG_PRINT("Connecting to Wi-Fi: ");
	DEBUG_PRINTLN(WIFI_SSID);

	WiFi.begin(WIFI_SSID, WIFI_PASS);

	unsigned long startTime = millis();
	while (WiFi.status() != WL_CONNECTED) {
		if (millis() - startTime > 15000) {
			DEBUG_PRINTLN("\nWiFi connection timeout");
			return false;
		}
		delay(500);
		DEBUG_PRINT(".");
	}

	DEBUG_PRINTLN("\nWiFi connected successfully");
	DEBUG_PRINT("IP address: ");
	DEBUG_PRINTLN(WiFi.localIP());
	return true;
}

/**
 * authbydeviceid - Authenticate to the API server using device credentials
 * Return: true if a token was obtained from either Set-Cookie header or JSON body
 */
bool authbydeviceid(void)
{
	DEBUG_PRINTLN("Starting user authentication...");

	WiFiClientSecure client;
	HTTPClient http;

	client.setInsecure();
	client.setTimeout(10000);
	if (!http.begin(client, TOKEN_URL)) {
		DEBUG_PRINTLN("Cannot connect to login server");
		return false;
	}

	http.setTimeout(10000);

	const char *headerKeys[] = { "Set-Cookie" };
	http.collectHeaders(headerKeys, 1);
	int httpCode = http.GET();

	bool authSuccess = false;
	DEBUG_PRINT(httpCode);
	if (httpCode == HTTP_CODE_OK) {
		DEBUG_PRINTLN("Login request successful");

		/* Try Set-Cookie header first */
		String setCookieHeader = http.header("Set-Cookie");
		DEBUG_PRINT(setCookieHeader);
		if (!setCookieHeader.isEmpty()) {
			authorizationToken =
				extractTokenFromHeader(setCookieHeader);
			if (!authorizationToken.isEmpty()) {
				DEBUG_PRINTLN("Token from header");
				authSuccess = true;
			}
		}

		/* Fall back to JSON body */
		if (!authSuccess) {
			String responseBody = http.getString();
			authorizationToken = extractTokenFromBody(responseBody);
			if (!authorizationToken.isEmpty()) {
				DEBUG_PRINTLN("Token from body");
				authSuccess = true;
			}
		}
	} else {
		DEBUG_PRINT("Login failed, HTTP code: ");
		DEBUG_PRINTLN(httpCode);
		if (httpCode < 0) {
			DEBUG_PRINT("Error message: ");
			DEBUG_PRINTLN(http.errorToString(httpCode).c_str());
		}
	}

	http.end();

	if (authSuccess) {
		DEBUG_PRINT("Auth token: ");
		DEBUG_PRINTLN(authorizationToken);
	} else {
		DEBUG_PRINTLN("Authentication failed: cannot retrieve token");
	}

	return authSuccess;
}

/**
 * sendSensorData - POST sensor JSON to the data endpoint
 * Return: true on 200/201, false on failure or missing auth token
 */
bool sendSensorData(void)
{
	if (authorizationToken.isEmpty()) {
		DEBUG_PRINTLN("Not authenticated, cannot send data");
		return false;
	}

	WiFiClientSecure client;
	HTTPClient http;

	client.setInsecure();

	if (!http.begin(client, DATA_URL)) {
		DEBUG_PRINTLN("Cannot connect to data server");
		return false;
	}

	http.addHeader("Content-Type", "application/json");
	http.addHeader("X-Device-Token", authorizationToken);
	http.setTimeout(10000);

	String jsonData = createSensorData();

	int httpCode = http.POST(jsonData);
	bool success = false;

	if (httpCode > 0) {
		DEBUG_PRINT("HTTP response code: ");
		DEBUG_PRINTLN(httpCode);

		String response = http.getString();
		DEBUG_PRINT("Server response: ");
		DEBUG_PRINTLN(response);

		if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_CREATED) {
			success = true;
		} else {
			DEBUG_PRINTLN("Data send failed");
			if (httpCode == HTTP_CODE_UNAUTHORIZED) {
				DEBUG_PRINTLN("Token expired");
				authorizationToken = "";
			}
		}
	} else {
		DEBUG_PRINT("Request failed, error code: ");
		DEBUG_PRINTLN(httpCode);
		DEBUG_PRINT("Error message: ");
		DEBUG_PRINTLN(http.errorToString(httpCode));
	}

	http.end();
	return success;
}

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

/**
 * extractTokenFromHeader - Parse X-Device-Token out of a Set-Cookie header
 * @setCookieHeader: raw Set-Cookie value
 * Return: token string, or empty string if not found
 */
String extractTokenFromHeader(const String &setCookieHeader)
{
	int authStart = setCookieHeader.indexOf("X-Device-Token=");
	if (authStart == -1)
		return "";

	int valueStart = authStart + strlen("X-Device-Token=");
	int valueEnd = setCookieHeader.indexOf(';', valueStart);
	if (valueEnd == -1)
		valueEnd = setCookieHeader.length();

	return setCookieHeader.substring(valueStart, valueEnd);
}

/**
 * extractTokenFromBody - Pull the device token from a JSON login response
 * @jsonBody: raw response body
 * Return: token from data.deviceToken, or empty string on parse failure
 */
String extractTokenFromBody(const String &jsonBody)
{
	cJSON *root = cJSON_Parse(jsonBody.c_str());

	if (root == NULL) {
		DEBUG_PRINT("JSON parse error: ");
		DEBUG_PRINTLN(cJSON_GetErrorPtr() ? cJSON_GetErrorPtr() :
						    "unknown error");
		return "";
	}

	String token = "";
	cJSON *data = cJSON_GetObjectItem(root, "data");
	if (data != NULL) {
		cJSON *deviceToken = cJSON_GetObjectItem(data, "deviceToken");
		if (deviceToken != NULL && cJSON_IsString(deviceToken))
			token = String(deviceToken->valuestring);
	}

	cJSON_Delete(root);
	return token;
}

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
			webSocket.sendTXT("{\"type\":\"response\",\"status\":\"parse_error\"}");
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
