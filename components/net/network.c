#include "network.h"
#include "common.h"
#include "token.h"

#include <limits.h>
#include <strings.h>
#include <string.h>
#include <stdlib.h>
#include "esp_http_client.h"
#include "esp_crt_bundle.h" /* IDF 内置 CA bundle */
#include "cJSON.h"
#include "display.h"
#include "esp_log.h"

static const char *TAG = "net";

/* HTTP 回调上下文：body + Set-Cookie */
typedef struct {
    char  *data;
    int    len;
    int    cap;
    char   set_cookie[512]; /* Token 优先从 Set-Cookie 取 */
} http_resp_buf_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_resp_buf_t *rb = (http_resp_buf_t *)evt->user_data;

    switch (evt->event_id) {
    case HTTP_EVENT_ON_HEADER:
        if (rb != NULL && evt->header_key != NULL &&
            strcasecmp(evt->header_key, "Set-Cookie") == 0) {
            if (evt->header_value != NULL) {
                strncpy(rb->set_cookie, evt->header_value,
                        sizeof(rb->set_cookie) - 1);
                rb->set_cookie[sizeof(rb->set_cookie) - 1] = '\0';
            }
        }
        break;

    case HTTP_EVENT_ON_DATA: {
        if (rb == NULL || rb->data == NULL)
            break;

        int room = rb->cap - rb->len;
        int copy = (evt->data_len < room) ? evt->data_len : room;
        if (copy > 0) {
            memcpy(rb->data + rb->len, evt->data, copy);
            rb->len += copy;
            rb->data[rb->len] = '\0';
        }
        break;
        
    }
    case HTTP_EVENT_ON_FINISH:
        if (rb != NULL)
            rb->len = 0;
        break;
    default:
        break;
    }
    return ESP_OK;
}

/* struct 较大，cfg 由调用方堆分配 */
static void make_http_config(esp_http_client_config_t *cfg,
                             const char *url,
                             http_resp_buf_t *rbuf)
{
    *cfg = (esp_http_client_config_t){
        .url = url,
        .crt_bundle_attach  = esp_crt_bundle_attach,
        .timeout_ms = 8000,
        .event_handler = http_event_handler,
        .user_data = rbuf,
        .buffer_size = NET_HTTP_OUTPUT_MAX,
    };
}

/* 所有接口都返回 HTTP 200，业务结果看 body.code；非法 body 返回 -1 */
static int parse_api_code(const char *body)
{
    if (body == NULL || body[0] == '\0')
        return -1;

    cJSON *root = cJSON_Parse(body);
    if (root == NULL)
    {
        ESP_LOGE(TAG, "Envelope parse error: %s",
                      cJSON_GetErrorPtr() ? cJSON_GetErrorPtr() : "unknown");
        return -1;
    }

    cJSON *code = cJSON_GetObjectItem(root, "code");
    int ret = (cJSON_IsNumber(code)) ? code->valueint : -1;
    cJSON_Delete(root);
    return ret;
}

/* 设备 ID + 引导密钥换 Token：先试 Set-Cookie，再回退 body */
bool authbydeviceid(void)
{

    ESP_LOGI(TAG, "Starting device authentication...");

    /* 主任务栈小，大缓冲一律堆分配 */
    char *buf_data = malloc(NET_HTTP_OUTPUT_MAX);
    http_resp_buf_t *rbuf = malloc(sizeof(http_resp_buf_t));
    esp_http_client_config_t *cfg = malloc(sizeof(esp_http_client_config_t));
    if (buf_data == NULL || rbuf == NULL || cfg == NULL) {
        ESP_LOGE(TAG, "OOM: cannot allocate HTTP buffers");
        free(buf_data); free(rbuf); free(cfg);
        return false;
    }
    *rbuf = (http_resp_buf_t){ .data = buf_data, .len = 0,
                                .cap = NET_HTTP_OUTPUT_MAX };
    make_http_config(cfg, TOKEN_URL, rbuf);
    ESP_LOGD(TAG, "HTTP URL = [%s]", cfg->url);
    esp_http_client_handle_t http = esp_http_client_init(cfg);
    esp_http_client_set_header(http, "X-Device-Secret", DEVICE_SECRET);

    esp_err_t err = esp_http_client_perform(http);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP perform failed: 0x%x", err);
        esp_http_client_cleanup(http);
        free(cfg); free(rbuf); free(buf_data);
        return false;
    }
    int httpCode = esp_http_client_get_status_code(http);
    bool authSuccess = false;

    ESP_LOGD(TAG, "HTTP status: %d", httpCode);

    /* HTTP 200 + code==200 才算成功 */
    if (httpCode == 200 && parse_api_code(rbuf->data) == 200) {
        ESP_LOGI(TAG, "Login request successful");

        char *new_token = NULL;

        if (strlen(rbuf->set_cookie) > 0) {
            new_token = extractTokenFromHeader(rbuf->set_cookie);
        }

        if (new_token == NULL && rbuf->len > 0) {
            new_token = extractTokenFromBody(rbuf->data);
        }

        if (new_token != NULL) {
            char *old_token = token_load();

            if (old_token != NULL && strcmp(old_token, new_token) == 0) {
                ESP_LOGI(TAG, "Token unchanged, skip saving");
            } else {
                token_save(new_token);
                ESP_LOGI(TAG, "Token updated (new token differs from old or old was NULL)");
            }

            free(old_token);
            free(new_token);
            authSuccess = true;
        } else {
            ESP_LOGE(TAG, "Failed to extract token from response");
            authSuccess = false;
        }
    } else {
        ESP_LOGE(TAG, "Authentication failed: http=%d body=%.120s",
                    httpCode, rbuf->data);
        authSuccess = false;
    }

    esp_http_client_cleanup(http);

    if (authSuccess) {
        char *tok = token_load();
        ESP_LOGD(TAG, "Auth token: %s", tok ? tok : "(null)");
        free(tok);
    }
    else
        ESP_LOGE(TAG, "Authentication failed: cannot retrieve token");

    free(cfg); free(rbuf); free(buf_data);
    return authSuccess;
}

/* 未知字段后端会忽略并返回 200，打印业务码便于发现未落库 */
bool reportPanelInfo(void)
{
    const char *chip = display_chip_id();
    const display_info_t *info = display_info();
    if (chip == NULL || info == NULL)
    {
        ESP_LOGD(TAG, "Panel not initialized, skip panel report");
        return false;
    }

    char *token = token_load();
    if (token == NULL || strlen(token) == 0)
    {
        free(token);
        return false;
    }

    cJSON *body = cJSON_CreateObject();
    if (body == NULL)
    {
        free(token);
        return false;
    }
    cJSON_AddStringToObject(body, "panelChip", chip);
    cJSON_AddNumberToObject(body, "panelWidth", info->width);
    cJSON_AddNumberToObject(body, "panelHeight", info->height);
    char *payload = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (payload == NULL)
    {
        free(token);
        return false;
    }

    char *buf_data = malloc(NET_HTTP_OUTPUT_MAX);
    http_resp_buf_t *rbuf = malloc(sizeof(http_resp_buf_t));
    esp_http_client_config_t *cfg = malloc(sizeof(esp_http_client_config_t));
    if (buf_data == NULL || rbuf == NULL || cfg == NULL)
    {
        ESP_LOGE(TAG, "Panel report: OOM");
        free(buf_data); free(rbuf); free(cfg);
        free(payload); free(token);
        return false;
    }
    *rbuf = (http_resp_buf_t){ .data = buf_data, .len = 0,
                                .cap = NET_HTTP_OUTPUT_MAX };
    make_http_config(cfg, DEVICE_UPDATE_URL, rbuf);
    esp_http_client_handle_t http = esp_http_client_init(cfg);
    esp_http_client_set_method(http, HTTP_METHOD_POST);
    esp_http_client_set_header(http, "X-Device-Token", token);
    esp_http_client_set_header(http, "Content-Type", "application/json");
    esp_http_client_set_post_field(http, payload, (int)strlen(payload));

    const esp_err_t err = esp_http_client_perform(http);
    const int httpCode = esp_http_client_get_status_code(http);
    esp_http_client_cleanup(http);

    bool ok = false;
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Panel report failed: %s", esp_err_to_name(err));
    }
    else if (httpCode == 200 && parse_api_code(rbuf->data) == 200)
    {
        ESP_LOGI(TAG, "Panel reported: %s %ux%u", chip, info->width,
                 info->height);
        ok = true;
    }
    else
    {
        ESP_LOGW(TAG, "Panel report rejected: http=%d body=%.120s", httpCode,
                 rbuf->data);
    }

    free(cfg); free(rbuf); free(buf_data);
    free(payload); free(token);
    return ok;
}

/* 拉取即消费，必须按 id 升序入队，否则会丢离线命令 */
bool syncPendingCommands(void)
{
    char *token = token_load();
    if (token == NULL || strlen(token) == 0) {
        free(token);
        return false;
    }

    char *buf_data = malloc(NET_HTTP_OUTPUT_MAX);
    http_resp_buf_t *rbuf = malloc(sizeof(http_resp_buf_t));
    esp_http_client_config_t *cfg = malloc(sizeof(esp_http_client_config_t));
    if (buf_data == NULL || rbuf == NULL || cfg == NULL) {
        ESP_LOGE(TAG, "Sync: OOM");
        free(buf_data); free(rbuf); free(cfg); free(token);
        return false;
    }
    *rbuf = (http_resp_buf_t){ .data = buf_data, .len = 0,
                                .cap = NET_HTTP_OUTPUT_MAX };
    make_http_config(cfg, DATA_PULL_URL, rbuf);
    esp_http_client_handle_t http = esp_http_client_init(cfg);

    esp_http_client_set_header(http, "X-Device-Token", token);

    esp_err_t err = esp_http_client_perform(http);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Sync HTTP perform failed: 0x%x", err);
        esp_http_client_cleanup(http);
        free(cfg); free(rbuf); free(buf_data); free(token);
        return false;
    }
    int httpCode = esp_http_client_get_status_code(http);

    esp_http_client_cleanup(http);
    free(token);

    /* HTTP 200 只代表到达 */
    int apiCode = parse_api_code(rbuf->data);
    if (httpCode != 200 || apiCode != 200) {
        ESP_LOGW(TAG, "Sync: http=%d code=%d body=%.160s",
                    httpCode, apiCode, rbuf->data);
        if (apiCode == 401) {
            ESP_LOGW(TAG, "Sync: device token invalid/expired, clearing local token");
            token_clear();
        }
        free(cfg); free(rbuf); free(buf_data);
        return false;
    }

    ESP_LOGD(TAG, "Sync response: %s", rbuf->data);

    cJSON *root = cJSON_Parse(rbuf->data);
    free(cfg); free(rbuf); free(buf_data);

    if (root == NULL) {
        ESP_LOGE(TAG, "Sync: JSON parse error");
        return false;
    }

    cJSON *arr = cJSON_GetObjectItem(root, "data");
    if (arr == NULL || !cJSON_IsArray(arr)) {
        ESP_LOGI(TAG, "Sync: no data array");
        cJSON_Delete(root);
        return false;
    }

    if (g_commandQueue == NULL)    {
        ESP_LOGW(TAG, "Sync: command queue not ready, skip enqueue");
        cJSON_Delete(root);
        return false;
    }
    int lastId = -1;
    int enqueued = 0;
    for (;;) {
        cJSON *item;
        cJSON *next = NULL;
        int nextId = INT_MAX;
        cJSON_ArrayForEach(item, arr) {
            cJSON *cid = cJSON_GetObjectItem(item, "id");
            if (cid != NULL && cJSON_IsNumber(cid) &&
                cid->valueint > lastId && cid->valueint < nextId) {
                nextId = cid->valueint;
                next = item;
            }
        }
        if (next == NULL)
            break;

        char *cmdStr = cJSON_PrintUnformatted(next);
        if (cmdStr == NULL) {
            ESP_LOGE(TAG, "Sync: serialization failed for cmd id=%d", nextId);
            break;
        }
        CommandMsg cmdMsg;
        cmdMsg.payload = cmdStr;
        cmdMsg.length = strlen(cmdStr);

        /* 队列满则丢弃剩余，避免死锁 */
        if (xQueueSend(g_commandQueue, &cmdMsg, pdMS_TO_TICKS(5000)) != pdPASS) {
            ESP_LOGW(TAG, "Sync: queue full, dropping remaining commands");
            free(cmdStr);
            break;
        }
        enqueued++;
        lastId = nextId;
    }
    ESP_LOGI(TAG, "Sync: enqueued %d pending command(s)", enqueued);
    cJSON_Delete(root);
    return enqueued > 0;
}

/* 从 Set-Cookie 取 X-Device-Token，返回 malloc 串 */
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
    if (root == NULL) {
        ESP_LOGE(TAG, "JSON parse error: %s",
                     cJSON_GetErrorPtr() ? cJSON_GetErrorPtr() : "unknown");
        return NULL;
    }

    char *token = NULL;
    cJSON *data = cJSON_GetObjectItem(root, "data");
    if (data != NULL) {
        cJSON *deviceToken = cJSON_GetObjectItem(data, "deviceToken");
        if (deviceToken != NULL && cJSON_IsString(deviceToken)) {
            token = strdup(deviceToken->valuestring);
        }
    }

    cJSON_Delete(root);
    return token;
}
