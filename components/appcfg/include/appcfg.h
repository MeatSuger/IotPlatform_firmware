#ifndef _APPCFG_H_
#define _APPCFG_H_

/* --------------------------------------------------------------------------
 * appcfg — 云端设备配置（MQTT retained 快照）的本地存储与应用状态机。
 *
 * 协议（backend api/swagger/API.md §6.3）：
 *   订阅  iot/{deviceId}/config        payload {"version":N,"config":{...}}
 *   发布  iot/{deviceId}/config/report payload {"version":N,"config":{...}}
 *
 * 配置分区生效（payload 分区语义见 API.md §4.5）：
 *   - sensor.reportInterval（秒）→ g_reportIntervalMs（传感器上报周期）
 *   - actuators[]（执行器定义数组）→ periph_apply_config() diff 实例化/卸载
 *     （设备类型 = 定义 config.transport：gpio/pwm/spi/led_strip，见 docs/mqtt-api.md）
 *   - sensors[]（传感器定义数组）→ app 侧按 type 采集器读取并周期上报
 *   - 其余分区（network/camera/ota 等）不生效但随 payload 原样持久化与回执
 *
 * 持久化（NVS namespace "appcfg"）：
 *   version  已应用版本（u32）
 *   payload  已应用 config 的 JSON 原文（string）—— 执行器定义的唯一真源
 *   rpending 回执未发出标志（u8）——回执发布成功后才清除，重启/重连后可补发
 * -------------------------------------------------------------------------- */

#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"

/* 启动时从 NVS 加载已应用状态（nvs_flash_init 之后、MQTT 启动之前调用一次） */
void appcfg_init(void);

/* 已应用版本号（默认 0 = 从未应用） */
uint32_t appcfg_version(void);

/* 已应用配置 payload 的 JSON 原文（内存态；无记录返回 NULL，不属于调用方） */
const char *appcfg_payload(void);

/* 是否有未发出的回执（应用成功但 report 发布失败/掉线） */
bool appcfg_report_pending(void);

/* 用内存中已持久化的 payload 重放运行时效果（上报周期 + 执行器定义）。
 * 供开机恢复调用（periph_bus_init 之后），修复“重启后等待云端推新版本才生效”
 * 的缺口；新版本到达时由 appcfg_handle_config 内部调用，二者共用同一实现。 */
void appcfg_replay(void);

/* 处理一条云端配置（MQTT iot/{id}/config 消息，version + config 对象）：
 *  - 新版本：校验 → NVS 持久化 → 应用运行时效果 → 发回执
 *  - 旧版本：忽略
 *  - 相同版本且有待回执：用已存 payload 补发回执（retained 重投的幂等恢复） */
void appcfg_handle_config(uint32_t version, const cJSON *config);

/* 尝试补发待回执（MQTT 重连/周期兜底；无待回执时为空操作） */
void appcfg_flush_pending_report(void);

#endif /* _APPCFG_H_ */
