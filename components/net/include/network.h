#ifndef _NETWORK_H_
#define _NETWORK_H_

#include <stdbool.h>

/* 命令轮询的响应缓冲上限 */
#define NET_HTTP_OUTPUT_MAX 8192

/* 用 DEVICE_ID + X-Device-Secret 换设备 Token 并保存 */
bool authbydeviceid(void);

/* 拉取平台命令队列，按 id 升序入队；返回是否至少入队一条 */
bool syncPendingCommands(void);

/* 上报面板芯片/分辨率：POST /devices/{id}/update（设备 Token） */
bool reportPanelInfo(void);

char *extractTokenFromHeader(const char *setCookieHeader);
char *extractTokenFromBody(const char *jsonBody);

#endif /* _NETWORK_H_ */
