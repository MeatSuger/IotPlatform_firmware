#ifndef _TOKEN_H_
#define _TOKEN_H_

#include <stdbool.h>

/* Load token from NVS. Returns malloc'd string, or NULL if not found.
 * Caller must free. */
char *token_load(void);

/* Save token to NVS. Returns true on success. */
bool token_save(const char *token);

/* Delete token from NVS. */
void token_clear(void);

#endif /* _TOKEN_H_ */
