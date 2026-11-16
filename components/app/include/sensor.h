#ifndef _SENSOR_H_
#define _SENSOR_H_

#include <stdbool.h>

typedef struct cJSON cJSON;

/* 传感器采集：由 config.sensors[] 定义驱动，按 type 匹配采集器，
 * 组装 {"sensors":[{name,type,value}...]} 发布到 telemetry。
 * dataType 不符的数据点会被平台整条丢弃，固件先自检并跳过。 */

/* def 为 sensors[] 单条定义；返回 value 节点，失败返回 NULL */
typedef cJSON *(*sensor_collect_fn)(const cJSON *def);

/* 同 type 重复注册会覆盖 */
bool sensor_register(const char *type, sensor_collect_fn fn);

/* 注册内置 temperature 采集器 */
bool sensor_init(void);

/* 返回 malloc 的 JSON，调用方 free；无任何产出时返回 NULL */
char *sensor_build_report(const char *config_json);

#endif /* _SENSOR_H_ */
