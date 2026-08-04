#ifndef _MAIN_H_
#define _MAIN_H_

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "mqtt_client.h"

/* --------------------------------------------------------------------------
 * URL & credential constants
 * -------------------------------------------------------------------------- */
#define API_HOST          "api.meatsuger.top"
#define DEVICE_ID         "90431b"
#define WIFI_SSID         "C9-1101"
#define WIFI_PASS         "printf%d1"
#define DATA_URL          "https://" API_HOST "/api/data/" DEVICE_ID "/Data"
#define DATA_PULL_URL     "https://" API_HOST "/api/device/" DEVICE_ID "/cmd"
#define TOKEN_URL         "https://" API_HOST "/api/device/" DEVICE_ID "/login"

/* MQTT broker over WSS — adjust port/path to match your server */
#define MQTT_BROKER_URI   "wss://" API_HOST "/api/ws/mqtt/broker"

/* MQTT topics */
#define MQTT_TOPIC_CMD    "device/" DEVICE_ID "/cmd"
#define MQTT_TOPIC_RESP   "device/" DEVICE_ID "/resp"

#define SEND_INTERVAL     30000U

/* --------------------------------------------------------------------------
 * Debug macros — controlled by DEBUG_ENABLE
 * -------------------------------------------------------------------------- */
#define DEBUG_ENABLE    1

#if DEBUG_ENABLE
#define DEBUG_PRINT(fmt, ...)   printf(fmt, ##__VA_ARGS__)
#define DEBUG_PRINTLN(fmt, ...) printf(fmt "\n", ##__VA_ARGS__)
#else
#define DEBUG_PRINT(fmt, ...)   do { } while(0)
#define DEBUG_PRINTLN(fmt, ...) do { } while(0)
#endif

#define TAG "firmware"

/* --------------------------------------------------------------------------
 * Shared state — defined in main.c
 * -------------------------------------------------------------------------- */
extern esp_mqtt_client_handle_t g_mqttClient;
extern bool g_isMQTTConnected;
extern QueueHandle_t g_commandQueue;

/* --------------------------------------------------------------------------
 * struct CommandMsg — FreeRTOS queue element for incoming commands.
 * -------------------------------------------------------------------------- */
typedef struct {
    char   *payload;  /* malloc'd JSON string; owned by the consumer */
    size_t  length;
} CommandMsg;

/* --------------------------------------------------------------------------
 * Event group bits for WiFi connection sync
 * -------------------------------------------------------------------------- */
#define WIFI_CONNECTED_BIT   BIT0
#define WIFI_FAIL_BIT        BIT1
#define NETWORK_INIT_DONE_BIT BIT2
extern EventGroupHandle_t g_wifiEventGroup;
extern bool g_authSuccess;

#endif /* _MAIN_H_ */
