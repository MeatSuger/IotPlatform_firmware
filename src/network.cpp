#include "network.h"
#include "sensor.h"

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
 * syncPendingCommands - Fetch pending commands on boot, enqueue the latest
 * Return: true if a command was found and enqueued
 */
bool syncPendingCommands(void)
{
	if (authorizationToken.isEmpty())
		return false;

	WiFiClientSecure client;
	HTTPClient http;

	client.setInsecure();
	client.setTimeout(10000);

	if (!http.begin(client, DATA_PULL_URL)) {
		DEBUG_PRINTLN("Sync: cannot connect to data server");
		return false;
	}

	http.addHeader("X-Device-Token", authorizationToken);

	int httpCode = http.GET();
	if (httpCode != HTTP_CODE_OK) {
		DEBUG_PRINT("Sync: HTTP error ");
		DEBUG_PRINTLN(httpCode);
		http.end();
		return false;
	}

	String response = http.getString();
	http.end();

	cJSON *root = cJSON_Parse(response.c_str());
	if (root == NULL) {
		DEBUG_PRINTLN("Sync: JSON parse error");
		return false;
	}

	cJSON *arr = cJSON_GetObjectItem(root, "data");
	if (arr == NULL || !cJSON_IsArray(arr)) {
		DEBUG_PRINTLN("Sync: no data array");
		cJSON_Delete(root);
		return false;
	}

	/* Find the entry with the highest id */
	cJSON *item;
	cJSON *latest = NULL;
	int maxId = -1;
	cJSON_ArrayForEach(item, arr) {
		cJSON *cid = cJSON_GetObjectItem(item, "id");
		if (cid != NULL && cJSON_IsNumber(cid) &&
		    cid->valueint > maxId) {
			maxId = cid->valueint;
			latest = item;
		}
	}

	if (latest == NULL) {
		DEBUG_PRINTLN("Sync: no pending commands");
		cJSON_Delete(root);
		return false;
	}

	/* Serialize and enqueue */
	char *cmdStr = cJSON_PrintUnformatted(latest);
	CommandMsg cmdMsg;
	cmdMsg.payload = cmdStr;
	cmdMsg.length = strlen(cmdStr);
	xQueueSend(commandQueue, &cmdMsg, portMAX_DELAY);
	DEBUG_PRINTF("Sync: enqueued cmd id=%d\n", maxId);

	/*
	 * cmdStr is leaked intentionally — it lives on the heap and
	 * the cmdProcessTask will cJSON_Parse a copy anyway.
	 * Only called once at boot, so the leak is bounded.
	 */

	cJSON_Delete(root);
	return true;
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
