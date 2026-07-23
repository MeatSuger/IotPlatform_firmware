#ifndef _MAIN_H_
#define _MAIN_H_

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <cJSON.h>
#include <WiFiClientSecure.h>
#include <WebSocketsClient.h>

/*
 * URL constants — composed at compile time via string literal concatenation.
 */
#define API_HOST      "api.meatsuger.top"
#define DEVICE_ID     "90431b"
#define WIFI_SSID     "C9-1101"
#define WIFI_PASS     "printf%d1"
#define DATA_URL      "https://" API_HOST "/api/data/" DEVICE_ID "/Data"
#define DATA_PULL_URL      "https://" API_HOST "/api/device/" DEVICE_ID "/cmd"
#define WSS_HOST      API_HOST
#define WSS_URL       "/api/ws/device"
#define TOKEN_URL     "https://" API_HOST "/api/device/" DEVICE_ID "/login"
#define SEND_INTERVAL 3000U

#define DEBUG_ENABLE 1
#if DEBUG_ENABLE
#define DEBUG_PRINT(...)   Serial.print(__VA_ARGS__)
#define DEBUG_PRINTF(...)  Serial.printf(__VA_ARGS__)
#define DEBUG_PRINTLN(...) Serial.println(__VA_ARGS__)
#else
#define DEBUG_PRINT(...)
#define DEBUG_PRINTF(...)
#define DEBUG_PRINTLN(...)
#endif

/*
 * Shared state — defined in main.cpp.
 */
extern String authorizationToken;
extern WebSocketsClient webSocket;
extern bool isWSConnected;
extern QueueHandle_t commandQueue;

/**
 * struct CommandMsg - FreeRTOS queue element for WebSocket commands.
 * @payload: pointer to raw JSON string, valid only until next WS loop iteration.
 * @length:  payload byte count.
 */
struct CommandMsg {
	char *payload;
	size_t length;
};

void deepSleepIfPossible(void);
void webSocketEvent(WStype_t type, uint8_t *payload, size_t length);

#endif /* _MAIN_H_ */
