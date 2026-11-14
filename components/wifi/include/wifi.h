#ifndef _WIFI_H_
#define _WIFI_H_

#include <stdbool.h>

/* 配网完成标记的 NVS 位置 */
#define WIFI_NVS_NS "wifi"
#define WIFI_NVS_KEY_PROVISIONED "provisioned"

/* WiFi STA：初始化、连接、SmartConfig 配网、事件通知（细节见 wifi.c）。
 * 无"已配网"标记（NVS "wifi"/"provisioned"）→ SC_TYPE_ESPTOUCH_AIRKISS
 * 配网（ESP-Touch + AirKiss，不含 v2）：手机 App 广播的凭据经
 * esp_wifi_set_config 持久化后连接；拿到 IP 才置标记，避免把连不上的
 * 错误网络固化成下次直连对象。普通模式 15 s 超时，配网模式阻塞至连上；
 * 成功置 WIFI_CONNECTED_BIT（g_wifiEventGroup，见 common.h），掉线自动重连。 */
bool wifi_init_sta(void);

#endif /* _WIFI_H_ */