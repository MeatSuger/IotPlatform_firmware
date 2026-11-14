#ifndef _APP_H_
#define _APP_H_

/* FreeRTOS 应用任务（在 app_main 中创建） */
void sensorReportTask(void *pvParameters);
void cmdProcessTask(void *pvParameters);

#endif /* _APP_H_ */
