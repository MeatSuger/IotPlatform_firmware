#ifndef _NETWORK_H_
#define _NETWORK_H_

#include <stdbool.h>

/* 兜底命令轮询的 HTTP 响应缓冲上限（字节） */
#define NET_HTTP_OUTPUT_MAX 8192

/* 设备认证：DeviceIDAuth（仅需路径 deviceId，无需用户登录），换取并保存设备
 * Token；网络失败 / 信封 code != 200 时返回 false。 */
bool authbydeviceid(void);

/* 兜底轮询平台命令队列（GET /commands，DeviceAuth）：条目按 id 升序入队，
 * 与 MQTT iot/{id}/cmd 实时通道互补（开机 / 重连后）；返回是否至少入队一条。 */
bool syncPendingCommands(void);

/* 上报面板芯片/分辨率：POST /devices/{id}/update（设备 Token） */
bool reportPanelInfo(void);

char *extractTokenFromHeader(const char *setCookieHeader);
char *extractTokenFromBody(const char *jsonBody);

#endif /* _NETWORK_H_ */
