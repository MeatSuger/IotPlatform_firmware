#ifndef _SENSOR_H_
#define _SENSOR_H_

#include <stdbool.h>

typedef struct cJSON cJSON;

/* 传感器采集 —— 由云端物模型定义（config.sensors[]）驱动，采集器注册表按定义 type 匹配。
 * 上报契约：sensors[].name = 定义 id，sensors[].type = 定义 type（后端无枚举校验、类型开放），
 * value 由匹配 type 的采集器产出，组装 {"sensors":[...]} 发布到 iot/{id}/telemetry。
 * 固件侧“读取类”设备按 type 注册采集器（内置 temperature → ESP32 内部温度 esp_driver_tsens，
 * 未来 i2c/spi 等按需注册），不做器件语义换算；平台按定义 dataType 校验上报值
 * （float/int=number、bool=布尔、text/enum=字符串），类型不符的数据点会被平台整条丢弃，
 * 故固件在源头自检（见 value_matches_datatype）：不符则跳过该点并告警。
 */

/* 采集器：def 为云端 sensors[] 单条定义（含 id/type/enabled/...）；返回新建的 value JSON
 * 节点（number/string/bool），由调用方 attach/释放；采集失败返回 NULL（该定义本轮跳过）。 */
typedef cJSON *(*sensor_collect_fn)(const cJSON *def);

/* 注册 type → 采集器（重复注册同 type 覆盖）。 */
bool sensor_register(const char *type, sensor_collect_fn fn);

/* 初始化内置采集器（temperature → 内部温度传感器）。 */
bool sensor_init(void);

/* 按已应用 config 原文（appcfg_payload()）的 "sensors" 数组逐条采集，输出
 * {"sensors":[{name,type,value}...]}（malloc，调用方 free）；enabled=false 或无匹配
 * 采集器/采集失败则跳过该条，无任何产出时返回 NULL（调用方不上报）。 */
char *sensor_build_report(const char *config_json);

#endif /* _SENSOR_H_ */
