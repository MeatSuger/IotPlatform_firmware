#ifndef _COMMON_H_
#define _COMMON_H_

/* Common types and shared state — included by every module.
 * 编译期常量见 config.h。 */

#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "config.h"

/* 仅作 include 顺序前导：task.h/queue.h/event_groups.h 依赖其先被包含（内核头 #error 校验） */
#include "freertos/FreeRTOS.h" /* IWYU pragma: keep */
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

/* BIT0..BIT5 定义于 esp_bit_defs.h；esp_system.h 本身未被使用，故直接包含前者 */
#include "esp_bit_defs.h"

/* 共享状态定义在 main/app_main.c；MQTT 客户端句柄/连接标志属 mqtt 组件，声明见
 * mqtt_app.h（避免 common.h 引入对 mqtt_client.h 的依赖） */
extern QueueHandle_t g_commandQueue;

typedef struct {
    char   *payload;  /* malloc'd JSON string; owned by the consumer */
    size_t  length;
} CommandMsg;

/* g_wifiEventGroup 位图（新增时同步维护此表）：
 * BIT0 WIFI_CONNECTED 已连接（GOT_IP 置位/断线清位）；BIT1 预留；BIT2 NETWORK_INIT_DONE
 * 认证+同步完成（spawn 前清、等待时 clear-on-exit）；BIT3 MQTT_AUTH_REFUSED CONNACK 拒绝
 * （= Token 失效）；BIT4 WIFI_DISCONNECTED WiFi 掉线；BIT5 RECOVERY_DONE 恢复完成。 */
#define WIFI_CONNECTED_BIT    BIT0
#define NETWORK_INIT_DONE_BIT BIT2
#define MQTT_AUTH_REFUSED_BIT  BIT3
#define WIFI_DISCONNECTED_BIT  BIT4
#define RECOVERY_DONE_BIT      BIT5
extern EventGroupHandle_t g_wifiEventGroup;
/* 跨任务读写（Recovery/NetInit 任务写、主任务读）：volatile 防缓存优化错读 */
extern volatile bool g_authSuccess;

/* 传感器上报周期（ms），运行时值：默认 SEND_INTERVAL，云端配置可覆盖 */
extern uint32_t g_reportIntervalMs;

#endif /* _COMMON_H_ */
