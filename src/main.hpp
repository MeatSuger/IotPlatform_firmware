#ifndef _MAIN_HPP_
#define _MAIN_HPP_

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <cJSON.h>
#include <WiFiClientSecure.h>
#include <WebSocketsClient.h>

// --- Constants ---
#define API_HOST "api.meatsuger.top"
#define DEVICE_ID "90431b"
#define WIFI_SSID "C9-1101"
#define WIFI_PASS "printf%d1"
#define DATA_URL "https://" API_HOST "/api/data/" DEVICE_ID "/Data"
#define WSS_HOST API_HOST
#define WSS_URL "/api/ws/device"
#define TOKEN_URL "https://" API_HOST "/api/device/" DEVICE_ID "/login"
#define SEND_INTERVAL 3000U

// --- Globals ---
extern String authorizationToken;
extern WebSocketsClient webSocket;
extern bool isWSConnected;

// --- Functions ---
bool connectToWiFi();
bool authbydeviceid();
bool sendSensorData();
String createSensorData();
String extractTokenFromBody(const String &jsonBody);
cJSON *extractCmdFromBody(const String &jsonBody);
String extractTokenFromHeader(const String &setCookieHeader);
void deepSleepIfPossible();
void httpUploadTask(void *pvParameters);
void webSocketEvent(WStype_t type, uint8_t *payload, size_t length);

#endif
