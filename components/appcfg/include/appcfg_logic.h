#ifndef _APPCFG_LOGIC_H_
#define _APPCFG_LOGIC_H_

#include <stdbool.h>
#include <stdint.h>

/* 是否需要应用/重放该版本：旧版本忽略；同版本已回执且有 payload 时幂等跳过；
 * 同版本待回执，或版本在但 payload 丢失(NVS 损坏/升级)时重新应用。 */
bool appcfg_should_apply(uint32_t version, uint32_t applied_version,
                         bool report_pending, bool has_payload);

#endif /* _APPCFG_LOGIC_H_ */
