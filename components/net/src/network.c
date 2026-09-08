#include "network.h"
#include "common.h"
#include "token.h"

#include <limits.h> /* INT_MAX */
#include <strings.h>
#include <string.h>
#include <stdlib.h>
#include "esp_http_client.h"
#include "esp_crt_bundle.h" /* esp_crt_bundle_attach (IDF bundled CA bundle) */
#include "cJSON.h"
#include "esp_log.h"

static const char *TAG = "net";

/* Max HTTP response body to buffer */
#define MAX_HTTP_OUTPUT_BUFFER 8192

/* --------------------------------------------------------------------------
 * http_resp_buf_t — response buffer passed as user_data to event handler.
 * -------------------------------------------------------------------------- */
typedef struct {
    char  *data;          /* body payload */
    int    len;           /* bytes written to data */
    int    cap;           /* total capacity of data[] */
    char   set_cookie[512]; /* captured Set-Cookie response header */
} http_resp_buf_t;

/* --------------------------------------------------------------------------
 * HTTP event handler — captures body + Set-Cookie header.
 * -------------------------------------------------------------------------- */
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

/* --------------------------------------------------------------------------
 * Internal helper: fill an esp_http_client_config_t with our defaults.
 * Caller allocates *cfg (heap preferred — struct is large).
 * -------------------------------------------------------------------------- */
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
        .buffer_size = MAX_HTTP_OUTPUT_BUFFER,
    };
}

/* --------------------------------------------------------------------------
 * parse_api_code — 解析平台统一响应信封 {"code":N,"message":"...","data":...} 的业务码。
 *
 * 平台约定：所有接口（含业务错误）均返回 HTTP 200，业务结果由 body.code 表达
 * （见 back/api/swagger/API.md §2.1）。因此 HTTP 状态码不能用于判定成功。
 * 返回：body 中 code 字段值；body 非法/非预期结构返回 -1。
 * -------------------------------------------------------------------------- */
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

/* --------------------------------------------------------------------------
 * authbydeviceid — authenticate to the API server using device credentials.
 *
 * Tries the Set-Cookie header first, then falls back to the JSON body.
 *
 * Returns: true if a token was obtained.
 * -------------------------------------------------------------------------- */
bool authbydeviceid(void)
{

    ESP_LOGI(TAG, "Starting device authentication...");

    /* Allocate everything on heap — main task stack is only 3584 bytes */
    char *buf_data = malloc(MAX_HTTP_OUTPUT_BUFFER);
    http_resp_buf_t *rbuf = malloc(sizeof(http_resp_buf_t));
    esp_http_client_config_t *cfg = malloc(sizeof(esp_http_client_config_t));
    if (buf_data == NULL || rbuf == NULL || cfg == NULL) {
        ESP_LOGE(TAG, "OOM: cannot allocate HTTP buffers");
        free(buf_data); free(rbuf); free(cfg);
        return false;
    }
    *rbuf = (http_resp_buf_t){ .data = buf_data, .len = 0,
                                .cap = MAX_HTTP_OUTPUT_BUFFER };
    make_http_config(cfg, TOKEN_URL, rbuf);
    ESP_LOGD(TAG, "HTTP URL = [%s]", cfg->url);
    esp_http_client_handle_t http = esp_http_client_init(cfg);

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

    /* 统一信封：HTTP 200 + body.code==200 才是业务成功 */
    if (httpCode == 200 && parse_api_code(rbuf->data) == 200) {
        ESP_LOGI(TAG, "Login request successful");

        char *new_token = NULL;

        if (strlen(rbuf->set_cookie) > 0) {
            /* 优先从 Set-Cookie 提取，否则从 JSON body 提取 */
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

            free(old_token);   /* token_load 返回 malloc 副本 */
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

/* --------------------------------------------------------------------------
 * syncPendingCommands — Fetch pending commands (HTTP GET /commands 兜底通道).
 *
 * 拉取即消费：平台返回 data 数组后即从队列删除，因此**全部**条目必须逐条
 * 入队处理（按 id 升序保证 config/control 时序），不能只取最新一条——
 * 否则离线期间累积的控制命令会被静默丢弃。与 MQTT iot/{id}/cmd 实时通道互补。
 *
 * Returns: true if at least one command was enqueued.
 * -------------------------------------------------------------------------- */
bool syncPendingCommands(void)
{
    char *token = token_load();
    if (token == NULL || strlen(token) == 0) {
        free(token);
        return false;
    }

    /* Allocate everything on heap — main task stack is only 3584 bytes */
    char *buf_data = malloc(MAX_HTTP_OUTPUT_BUFFER);
    http_resp_buf_t *rbuf = malloc(sizeof(http_resp_buf_t));
    esp_http_client_config_t *cfg = malloc(sizeof(esp_http_client_config_t));
    if (buf_data == NULL || rbuf == NULL || cfg == NULL) {
        ESP_LOGE(TAG, "Sync: OOM");
        free(buf_data); free(rbuf); free(cfg); free(token);
        return false;
    }
    *rbuf = (http_resp_buf_t){ .data = buf_data, .len = 0,
                                .cap = MAX_HTTP_OUTPUT_BUFFER };
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

    /* 业务结果看 body.code（HTTP 200 仅是到达） */
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

    /* Parse JSON */
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

    /* 按 id 升序逐条入队（拉取即消费：丢弃即永久丢失） */
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

        /* 超时入队：队列满说明处理任务未就绪/积压，丢弃剩余并退出
         * （已入队部分仍会被消费），避免永久死锁 */
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

/* --------------------------------------------------------------------------
 * extractTokenFromHeader — Parse X-Device-Token out of a Set-Cookie header.
 *
 * Returns: malloc'd token string, or NULL if not found.
 * -------------------------------------------------------------------------- */
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

/* --------------------------------------------------------------------------
 * extractTokenFromBody — Pull the device token from a JSON login response.
 *
 * Returns: malloc'd token from data.deviceToken, or NULL on parse failure.
 * -------------------------------------------------------------------------- */
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
