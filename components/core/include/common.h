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
 * Shared state — defined in main/app_main.c.
 * 注：MQTT 客户端句柄/连接标志属 mqtt 组件，声明见 mqtt_app.h（避免 common.h
 * 引入对 mqtt_client.h 的依赖）。
 * -------------------------------------------------------------------------- */
extern QueueHandle_t g_commandQueue;

/* MQTT 连接被 broker 拒绝（CONNACK 非 accepted，通常 = 设备 Token 失效/被清理）。
 * 置位后主循环应触发重新取 Token + 重建 MQTT 客户端（见 app_main recover_connection）。
 * 收到 CONNECTED 事件时清零。 */
extern volatile bool g_mqttAuthRefused;

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

/* 传感器上报周期（ms），运行时值：默认 SEND_INTERVAL，云端配置可覆盖 */
extern uint32_t g_reportIntervalMs;

#endif /* _COMMON_H_ */
