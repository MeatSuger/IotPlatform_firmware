#include "appcfg_logic.h"

bool appcfg_should_apply(uint32_t version, uint32_t applied_version,
                         bool report_pending, bool has_payload)
{
    if (version < applied_version)
        return false;
    return version > applied_version || report_pending || !has_payload;
}
