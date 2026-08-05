#ifndef _SENSOR_H_
#define _SENSOR_H_

#include <stdbool.h>
#include "common.h"
char *createSensorData(void);
bool sensor_init(void);

/* Build the sensor JSON and publish it to the MQTT response topic.
 * Returns false if the JSON build or the publish failed. */
bool sensor_publish_data(void);

#endif /* _SENSOR_H_ */
