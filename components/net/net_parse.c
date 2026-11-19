#include "net_parse.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

#include "network.h"

int parse_api_code(const char *body)
{
    if (body == NULL || body[0] == '\0')
        return -1;

    cJSON *root = cJSON_Parse(body);
    if (root == NULL)
        return -1;

    cJSON *code = cJSON_GetObjectItem(root, "code");
    int ret = (cJSON_IsNumber(code)) ? code->valueint : -1;
    cJSON_Delete(root);
    return ret;
}

/* 从 Set-Cookie 取 X-Device-Token,返回 malloc 串 */
char *extractTokenFromHeader(const char *setCookieHeader)
{
    const char *key = "X-Device-Token=";
    const char *found = strstr(setCookieHeader, key);
    if (found == NULL)
        return NULL;

    found += strlen(key);
    const char *end = strchr(found, ';');
    size_t len = end ? (size_t)(end - found) : strlen(found);

    char *token = malloc(len + 1);
    if (token == NULL)
        return NULL;
    memcpy(token, found, len);
    token[len] = '\0';
    return token;
}

/* 从 body.data.deviceToken 取 Token */
char *extractTokenFromBody(const char *jsonBody)
{
    cJSON *root = cJSON_Parse(jsonBody);
    if (root == NULL)
        return NULL;

    char *token = NULL;
    cJSON *data = cJSON_GetObjectItem(root, "data");
    if (data != NULL) {
        cJSON *deviceToken = cJSON_GetObjectItem(data, "deviceToken");
        if (deviceToken != NULL && cJSON_IsString(deviceToken))
            token = strdup(deviceToken->valuestring);
    }

    cJSON_Delete(root);
    return token;
}
