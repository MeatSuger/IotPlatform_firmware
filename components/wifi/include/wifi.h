#ifndef _WIFI_H_
#define _WIFI_H_

#include <stdbool.h>

/* 配网完成标记的 NVS 位置 */
#define WIFI_NVS_NS "wifi"
#define WIFI_NVS_KEY_PROVISIONED "provisioned"

/* STA：无已配网标记时进 SmartConfig(ESP-Touch + AirKiss),拿到 IP 才写标记；
 * 配网模式阻塞等待,普通模式 15s 超时；掉线自动重连。 */
bool wifi_init_sta(void);

#endif /* _WIFI_H_ */