#ifndef _SENSOR_H_
#define _SENSOR_H_

#include <stdbool.h>

typedef struct cJSON cJSON;

/* --------------------------------------------------------------------------
 * 传感器采集 —— 由云端物模型定义（config.sensors[]）驱动。
 *
 * 上报契约（与平台对齐）：
 *   sensors[].name = 定义 id；sensors[].type = 定义 type（后端无枚举校验，
 *   温度/湿度等类型完全开放）；value 由匹配 type 的采集器产出。
 *
 * 固件侧“读取类”设备按 type 注册采集器（temperature → ESP32 内部温度，
 * 未来 i2c/spi 等读类器件按需注册新采集器）；固件不做器件语义换算。
 * -------------------------------------------------------------------------- */

/* 采集器：def 为云端 sensors[] 单条定义（含 id/type/enabled/...）。
 * 返回新建的 value JSON 节点（number/string/bool），由调用方 attach/释放；
 * 采集失败返回 NULL（该定义本轮跳过，不中断其它传感器）。 */
typedef cJSON *(*sensor_collect_fn)(const cJSON *def);

/* 注册 type → 采集器（重复注册同 type 覆盖）。 */
bool sensor_register(const char *type, sensor_collect_fn fn);

/* 初始化内置采集器（temperature → 内部温度传感器）。 */
bool sensor_init(void);

/* 按云端配置生成上报 JSON：config_json 为已应用 config 对象原文
 * （appcfg_payload() 返回值），取其中 "sensors" 定义数组逐条采集：
 *   enabled=false 跳过；type 无采集器 → 告警并跳过；
 * 输出 {"sensors":[{name,type,value}...]}（malloc，调用方 free）；
 * 无 sensors 定义或全部采集失败 → 返回 NULL（调用方不上报）。 */
char *sensor_build_report(const char *config_json);

#endif /* _SENSOR_H_ */
