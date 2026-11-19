#ifndef _NET_PARSE_H_
#define _NET_PARSE_H_

/* 纯 C 解析逻辑：固件与 host 测试共用,不依赖 ESP-IDF。 */

/* 所有接口都返回 HTTP 200,业务结果看 body.code；非法 body 返回 -1 */
int parse_api_code(const char *body);

#endif /* _NET_PARSE_H_ */
