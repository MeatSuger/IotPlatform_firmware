#ifndef _APP_H_
#define _APP_H_

#include "common.h"

/* FreeRTOS application tasks (created in app_main). */
void sensorReportTask(void *pvParameters);
void cmdProcessTask(void *pvParameters);

#endif /* _APP_H_ */
