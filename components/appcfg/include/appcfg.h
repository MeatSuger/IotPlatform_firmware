#ifndef _APPCFG_H_
#define _APPCFG_H_

/* appcfg — 云端设备配置（MQTT retained 快照）的本地存储与应用状态机。
 * 协议（API.md §6.3）：订阅 iot/{deviceId}/config、发布 iot/{deviceId}/config/report，
 * payload 同形 {"version":N,"config":{...}}；新版本才应用、旧版本忽略、同版本有待回执则补发。
 * 生效分区：sensor.reportInterval(秒)→g_reportIntervalMs；actuators[]→periph_apply_config()
 * diff 实例化/卸载；sensors[]→app 侧按 type 采集器采集并周期上报；其余分区（network/camera/
 * ota 等）不生效但原样持久化与回执（分区字段语义见 API.md §4.5）。
 * NVS "appcfg"：version(u32)、payload(JSON 原文，执行器定义唯一真源)、rpending(回执未发出，
 * 发布成功才清除，重启/重连后可补发)；appcfg_replay() 在 periph_bus_init() 之后调用，
 * 重启后不等云端推新版本即可恢复现场。
 */

#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"

/* NVS 命名空间与 key（"appcfg" 下存已应用状态） */
#define APP_CFG_NVS_NS "appcfg"
#define APP_CFG_NVS_KEY_VERSION "version"
#define APP_CFG_NVS_KEY_PAYLOAD "payload"
#define APP_CFG_NVS_KEY_RPENDING "rpending"

/* 配置 payload 上限（NVS 单值上限 4000 字节留余量） */
#define APP_CFG_PAYLOAD_MAX 4095U

/* 启动时从 NVS 加载已应用状态（nvs_flash_init 之后、MQTT 启动之前调用一次） */
void appcfg_init(void);

/* 已应用版本号（默认 0 = 从未应用） */
uint32_t appcfg_version(void);

/* 已应用配置 payload 的 JSON 原文（内存态；无记录返回 NULL，不属于调用方） */
const char *appcfg_payload(void);

/* 是否有未发出的回执（应用成功但 report 发布失败/掉线） */
bool appcfg_report_pending(void);

/* 用已持久化 payload 重放运行时效果（上报周期 + 执行器定义）；在 periph_bus_init()
 * 之后调用，补上重启后不等云端推新版本就恢复现场的缺口（新版本应用走同一实现）。 */
void appcfg_replay(void);

/* 处理云端配置（version + config 对象）：新版本 校验→NVS 持久化→应用运行时效果→发回执；
 * 旧版本忽略；相同版本且有待回执 → 用已存 payload 补发回执（retained 重投的幂等恢复）。 */
void appcfg_handle_config(uint32_t version, const cJSON *config);

/* 尝试补发待回执（MQTT 重连/周期兜底；无待回执时为空操作） */
void appcfg_flush_pending_report(void);

#endif /* _APPCFG_H_ */
