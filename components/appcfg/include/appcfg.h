#ifndef _APPCFG_H_
#define _APPCFG_H_

/* 云端配置快照：订阅 config、持久化到 NVS "appcfg"，应用 reportInterval 与
 * actuators，其余分区只保存并回执。 */

#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"

/* NVS namespace 与 key */
#define APP_CFG_NVS_NS "appcfg"
#define APP_CFG_NVS_KEY_VERSION "version"
#define APP_CFG_NVS_KEY_PAYLOAD "payload"
#define APP_CFG_NVS_KEY_RPENDING "rpending"

/* nvs_set_str 上限 4000 字节(含 NUL)，配置 payload 最大 3999 */
#define APP_CFG_PAYLOAD_MAX 3999U

/* nvs_flash_init 之后、MQTT 启动前调用一次 */
void appcfg_init(void);

/* 已应用 JSON 原文的堆副本，调用方 free；无配置返回 NULL */
char *appcfg_payload(void);

/* 回执是否未发出(QoS1 PUBACK 到达后才清除) */
bool appcfg_report_pending(void);

/* 开机重放已保存的运行时效果，在 periph_bus_init() 之后调用 */
void appcfg_replay(void);

/* 新版本应用并回执；旧版本忽略；同版本待回执则补发 */
void appcfg_handle_config(uint32_t version, const cJSON *config);

/* 周期或重连后补发待回执(QoS1，收到 PUBACK 才认为送达) */
void appcfg_flush_pending_report(void);

#endif /* _APPCFG_H_ */
