#ifndef _MQTT_APP_H_
#define _MQTT_APP_H_

#include <stdbool.h>
#include <stddef.h>
#include "mqtt_client.h"

/* MQTT over WSS：username = 设备 ID，password = 设备 Token。
 * 订阅 cmd/config/img，发布 telemetry/config/report。 */

/* 定义于 main/app_main.c，由 mqtt 组件维护 */
extern esp_mqtt_client_handle_t g_mqttClient;
extern volatile bool g_isMQTTConnected;

/* 创建并配置客户端（不启动）；失败返回 NULL */
esp_mqtt_client_handle_t mqtt_app_create(void);

/* 启动客户端（异步连接） */
void mqtt_app_start(esp_mqtt_client_handle_t client);

/* stop 在重连等待态最长阻塞 30s，只能在专用任务调用 */
void mqtt_app_destroy(esp_mqtt_client_handle_t client);

/* 发布到任意主题 */
bool mqtt_publish(const char *topic, const char *payload, size_t len);

#endif /* _MQTT_APP_H_ */
