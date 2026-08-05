#ifndef _NETWORK_H_
#define _NETWORK_H_

#include "common.h"

bool connectToWiFi(void);   /* kept for compatibility; wifi init is in main.c */
bool authbydeviceid(void);
bool sendSensorData(void);
bool syncPendingCommands(void);
char *extractTokenFromHeader(const char *setCookieHeader);
char *extractTokenFromBody(const char *jsonBody);

#endif /* _NETWORK_H_ */
