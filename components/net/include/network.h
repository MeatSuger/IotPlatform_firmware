#ifndef _NETWORK_H_
#define _NETWORK_H_

#include "common.h"

/* 设备认证：DeviceIDAuth（仅需路径 deviceId）——获取/刷新设备 Token。
 * 返回 true 表示成功取得并保存 Token。 */
bool authbydeviceid(void);

/* 拉取并消费平台命令队列（GET /commands，DeviceAuth）：全部条目按 id 升序入队，
 * 与 MQTT iot/{id}/cmd 实时通道互补（开机 / 重连后兑底）。
 * 返回 true 表示至少入队一条。 */
bool syncPendingCommands(void);

char *extractTokenFromHeader(const char *setCookieHeader);
char *extractTokenFromBody(const char *jsonBody);

#endif /* _NETWORK_H_ */
