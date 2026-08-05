#ifndef _COMMON_H_
#define _COMMON_H_

/* --------------------------------------------------------------------------
 * Common types, shared state and logging macros — included by every
 * module. Compile-time constants live in config.h.
 * -------------------------------------------------------------------------- */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "config.h"

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
 * Debug macros — controlled by DEBUG_ENABLE (config.h)
 * -------------------------------------------------------------------------- */
#if DEBUG_ENABLE
#define DEBUG_PRINT(fmt, ...)   printf(fmt, ##__VA_ARGS__)
#define DEBUG_PRINTLN(fmt, ...) printf(fmt "\n", ##__VA_ARGS__)
#else
#define DEBUG_PRINT(fmt, ...)   do { } while(0)
#define DEBUG_PRINTLN(fmt, ...) do { } while(0)
#endif

#define TAG "firmware"

/* --------------------------------------------------------------------------
 * Shared state — defined in app_main.c
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
#define WIFI_CONNECTED_BIT    BIT0
#define WIFI_FAIL_BIT         BIT1
#define NETWORK_INIT_DONE_BIT BIT2
extern EventGroupHandle_t g_wifiEventGroup;
extern bool g_authSuccess;

#endif /* _COMMON_H_ */
