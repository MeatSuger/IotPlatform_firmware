#include "main.hpp"
#include <cstring>

// Global variables
String authorizationToken = "";
WebSocketsClient webSocket;

unsigned long lastSendTime = 0;
bool isWSConnected = false;

void setup()
{
	Serial.begin(115200);
	Serial.println("Device starting...");

	if (!connectToWiFi()) {
		Serial.println("WiFi connection failed, restarting device...");
		ESP.restart();
		return;
	}

	if (!authbydeviceid()) {
		Serial.println("Authentication failed, restarting device...");
		ESP.restart();
		return;
	}

	webSocket.beginSSL(WSS_HOST, 443, WSS_URL);
	webSocket.setExtraHeaders("Origin=https://" API_HOST);
	webSocket.setAuthorization(authorizationToken.c_str());
	webSocket.setReconnectInterval(5000);
	webSocket.onEvent(webSocketEvent);

	xTaskCreatePinnedToCore(
		httpUploadTask,
		"HttpUploadTask",
		10240,
		NULL,
		1,
		NULL,
		0
	);

	Serial.println("System init complete, Core 0 background task started");
}

void loop()
{
	if (WiFi.status() != WL_CONNECTED) {
		Serial.println("WiFi disconnected, attempting to reconnect...");
		isWSConnected = false;
		if (!connectToWiFi() || !authbydeviceid()) {
			Serial.println(
				"Reconnection failed, restarting device...");
			ESP.restart();
			return;
		}
		webSocket.setAuthorization(authorizationToken.c_str());
	}

	webSocket.loop();

	delay(50);
}

void webSocketEvent(WStype_t type, uint8_t *payload, size_t length)
{
	switch (type) {
		case WStype_DISCONNECTED:
			Serial.println("WebSocket disconnected");
			isWSConnected = false;
			break;
		case WStype_CONNECTED:
			Serial.println("WebSocket connected");
			isWSConnected = true;
			webSocket.sendTXT("Hello Server!");
			break;
		case WStype_TEXT: {
			Serial.printf("Message received: %s\n", (char *)payload);
			cJSON *cmdPayload =
				extractCmdFromBody(String((char *)payload));
			if (cmdPayload != NULL) {
				cJSON *gpio =
					cJSON_GetObjectItem(cmdPayload, "GPIO");
				cJSON *action =
					cJSON_GetObjectItem(cmdPayload, "action");
				if (gpio != NULL && cJSON_IsString(gpio) &&
					action != NULL && cJSON_IsString(action)) {
					int pin = atoi(gpio->valuestring);
					pinMode(pin, OUTPUT);
					if (strcmp(action->valuestring, "on") == 0) {
						digitalWrite(pin, HIGH);
						Serial.printf("GPIO %d -> ON\n",
								pin);
					} else if (strcmp(action->valuestring,
							"off") == 0) {
						digitalWrite(pin, LOW);
						Serial.printf("GPIO %d -> OFF\n",
								pin);
					} else if (strcmp(action->valuestring, "toggle") == 0) {
						digitalWrite(pin, !digitalRead(pin));
						Serial.printf("GPIO %d -> TOGGLE\n",
								pin);
					} else {
						Serial.printf(
							"Unknown action: %s\n",
							action->valuestring);
					}
				}
				cJSON_Delete(cmdPayload);
			}
			break;
		}
		case WStype_ERROR:
			Serial.printf("WebSocket error: %s\n", (char *)payload);
			break;
		case WStype_PING:
			Serial.println("Ping received");
			break;
		case WStype_PONG:
			Serial.println("Pong received");
			break;
		default:
			Serial.printf("Unknown event type: %s\n", (char *)payload);
			break;
	}
}

bool connectToWiFi()
{
	Serial.print("Connecting to Wi-Fi: ");
	Serial.println(WIFI_SSID);

	WiFi.begin(WIFI_SSID, WIFI_PASS);

	unsigned long startTime = millis();
	while (WiFi.status() != WL_CONNECTED) {
		if (millis() - startTime > 15000) {
			Serial.println("\nWiFi connection timeout");
			return false;
		}
		delay(500);
		Serial.print(".");
	}

	Serial.println("\nWiFi connected successfully");
	Serial.print("IP address: ");
	Serial.println(WiFi.localIP());
	return true;
}

bool authbydeviceid()
{
	Serial.println("Starting user authentication...");

	WiFiClientSecure client;
	HTTPClient http;

	client.setInsecure();
	client.setTimeout(10000);
	if (!http.begin(client, TOKEN_URL)) {
		Serial.println("Cannot connect to login server");
		return false;
	}

	http.setTimeout(10000);

	const char *headerKeys[] = { "Set-Cookie" };
	http.collectHeaders(headerKeys, 1);
	int httpCode = http.GET();

	bool authSuccess = false;
	Serial.print(httpCode);
	if (httpCode == HTTP_CODE_OK) {
		Serial.println("Login request successful");

		String setCookieHeader = http.header("Set-Cookie");
		Serial.print(setCookieHeader);
		if (!setCookieHeader.isEmpty()) {
			authorizationToken =
				extractTokenFromHeader(setCookieHeader);
			if (!authorizationToken.isEmpty()) {
				Serial.println(
					"Token retrieved from response header successfully");
				authSuccess = true;
			}
		}

		if (!authSuccess) {
			String responseBody = http.getString();
			authorizationToken = extractTokenFromBody(responseBody);
			if (!authorizationToken.isEmpty()) {
				Serial.println(
					"Token retrieved from response body successfully");
				authSuccess = true;
			}
		}
	} else {
		Serial.print("Login failed, HTTP code: ");
		Serial.println(httpCode);
		if (httpCode < 0) {
			Serial.print("Error message: ");
			Serial.println(http.errorToString(httpCode).c_str());
		}
	}

	http.end();

	if (authSuccess) {
		Serial.print("Auth token: ");
		Serial.println(authorizationToken);
	} else {
		Serial.println("Authentication failed: cannot retrieve token");
	}

	return authSuccess;
}

bool sendSensorData()
{
	if (authorizationToken.isEmpty()) {
		Serial.println("Not authenticated, cannot send data");
		return false;
	}

	WiFiClientSecure client;
	HTTPClient http;

	client.setInsecure();

	if (!http.begin(client, DATA_URL)) {
		Serial.println("Cannot connect to data server");
		return false;
	}

	http.addHeader("Content-Type", "application/json");
	http.addHeader("X-Device-Token", authorizationToken);
	http.setTimeout(10000);

	String jsonData = createSensorData();

	int httpCode = http.POST(jsonData);
	bool success = false;

	if (httpCode > 0) {
		Serial.print("HTTP response code: ");
		Serial.println(httpCode);

		String response = http.getString();
		Serial.print("Server response: ");
		Serial.println(response);

		if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_CREATED) {
			success = true;
		} else {
			Serial.println(
				"Data send failed, server returned error");

			if (httpCode == HTTP_CODE_UNAUTHORIZED) {
				Serial.println(
					"Token expired, re-authentication required");
				authorizationToken = "";
			}
		}
	} else {
		Serial.print("Request failed, error code: ");
		Serial.println(httpCode);
		Serial.print("Error message: ");
		Serial.println(http.errorToString(httpCode));
	}

	http.end();
	return success;
}

String createSensorData()
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

String extractTokenFromBody(const String &jsonBody)
{
	cJSON *root = cJSON_Parse(jsonBody.c_str());

	if (root == NULL) {
		Serial.print("JSON parse error: ");
		Serial.println(cJSON_GetErrorPtr() ? cJSON_GetErrorPtr() :
						     "unknown error");
		return "";
	}

	String token = "";
	cJSON *data = cJSON_GetObjectItem(root, "data");
	if (data != NULL) {
		cJSON *deviceToken = cJSON_GetObjectItem(data, "deviceToken");
		if (deviceToken != NULL && cJSON_IsString(deviceToken)) {
			token = String(deviceToken->valuestring);
		}
	}

	cJSON_Delete(root);
	return token;
}

cJSON *extractCmdFromBody(const String &jsonBody)
{
	cJSON *root = cJSON_Parse(jsonBody.c_str());

	if (root == NULL) {
		Serial.print("JSON parse error: ");
		Serial.println(cJSON_GetErrorPtr() ? cJSON_GetErrorPtr() :
						     "unknown error");
		return NULL;
	}

	cJSON *payload = cJSON_DetachItemFromObject(root, "payload");
	cJSON_Delete(root);

	if (payload == NULL) {
		Serial.println("No payload in command");
	}

	return payload;
}

void httpUploadTask(void *pvParameters)
{
	Serial.print("[Core 0] HTTP upload task started, running on core: ");
	Serial.println(xPortGetCoreID());

	for (;;) {
		if (WiFi.status() == WL_CONNECTED &&
		    !authorizationToken.isEmpty()) {
			if (!sendSensorData()) {
				Serial.println(
					"[Core 0] HTTP data upload failed");
			}
		}

		vTaskDelay(pdMS_TO_TICKS(SEND_INTERVAL));
	}
}
