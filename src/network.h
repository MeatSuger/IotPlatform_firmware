#ifndef _NETWORK_H_
#define _NETWORK_H_

#include "main.h"

bool connectToWiFi(void);
bool authbydeviceid(void);
bool sendSensorData(void);
bool syncPendingCommands(void);
String extractTokenFromHeader(const String &setCookieHeader);
String extractTokenFromBody(const String &jsonBody);

#endif /* _NETWORK_H_ */
