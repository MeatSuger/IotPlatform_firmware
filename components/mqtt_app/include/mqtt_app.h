#ifndef _MQTT_APP_H_
#define _MQTT_APP_H_

#include <stdbool.h>
#include <stddef.h>
#include "mqtt_client.h"

/* MQTT 应用层：客户端生命周期 + 发布助手。接入契约：MQTT over WSS，broker 路径
 * /api/ws/mqtt/broker，CONNECT 用户名 = 设备 ID、口令 = 设备 Token。订阅 `iot/{id}/cmd`
 * （QoS1 非 retained，入队分发）与 `iot/{id}/config`（retained，订阅即得最新快照）；
 * 上行遥测 `iot/{id}/telemetry`、配置回执 `iot/{id}/config/report`。publish 先快照句柄
 * （app_main 重连会 destroy/重建客户端）防 TOCTOU；create 注册回调。 */

/* MQTT 客户端句柄与连接状态（定义于 main/app_main.c，由 mqtt 组件独占维护）。
 * g_isMQTTConnected 写者 = MQTT 事件回调任务，读者 = 主任务/上报任务：volatile */
extern esp_mqtt_client_handle_t g_mqttClient;
extern volatile bool g_isMQTTConnected;

/* 创建并配置客户端（不启动）；失败返回 NULL */
esp_mqtt_client_handle_t mqtt_app_create(void);

/* 启动客户端（异步连接） */
void mqtt_app_start(esp_mqtt_client_handle_t client);

/* 停止并销毁：stop 在重连等待态最长阻塞约 30s，只能在专用恢复任务调用（否则饿死 IDLE 触发 WDT） */
void mqtt_app_destroy(esp_mqtt_client_handle_t client);

/* 发布到任意主题 */
bool mqtt_publish(const char *topic, const char *payload, size_t len);

#endif /* _MQTT_APP_H_ */
