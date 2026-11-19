#ifndef _COMMON_H_
#define _COMMON_H_

/* 公共类型与共享状态；常量见 config.h */

#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "config.h"

/* FreeRTOS.h 必须先包含(内核头有 #error 校验) */
#include "freertos/FreeRTOS.h" /* IWYU pragma: keep */
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

#include "esp_bit_defs.h"

/* 定义在 main/app_main.c */
extern QueueHandle_t g_commandQueue;

typedef struct {
    char   *payload;  /* malloc'd JSON string; owned by the consumer */
    size_t  length;
} CommandMsg;

/* g_wifiEventGroup：BIT0 WIFI_CONNECTED、BIT2 NETWORK_INIT_DONE、
 * BIT3 MQTT_AUTH_REFUSED、BIT4 WIFI_DISCONNECTED、BIT5 RECOVERY_DONE */
#define WIFI_CONNECTED_BIT    BIT0
#define NETWORK_INIT_DONE_BIT BIT2
#define MQTT_AUTH_REFUSED_BIT  BIT3
#define WIFI_DISCONNECTED_BIT  BIT4
#define RECOVERY_DONE_BIT      BIT5
extern EventGroupHandle_t g_wifiEventGroup;
extern volatile bool g_authSuccess;

/* 运行时上报周期,默认 SEND_INTERVAL */
extern uint32_t g_reportIntervalMs;

#endif /* _COMMON_H_ */
