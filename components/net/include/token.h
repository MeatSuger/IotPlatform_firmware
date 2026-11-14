#ifndef _TOKEN_H_
#define _TOKEN_H_

#include <stdbool.h>

/* 设备 Token 的 NVS 位置 */
#define TOKEN_NVS_NS "device"
#define TOKEN_NVS_KEY "auth_token"

/* 设备 Token 的 NVS 存取：load 返回 malloc 串（调用方 free），未找到 NULL；
 * save 失败返回 false。 */
char *token_load(void);
bool token_save(const char *token);
void token_clear(void);

#endif /* _TOKEN_H_ */
