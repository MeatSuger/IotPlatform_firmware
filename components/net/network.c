#include "network.h"
#include "common.h"
#include "net_parse.h"
#include "token.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_crt_bundle.h" /* IDF 内置 CA bundle */
#include "esp_http_client.h"
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "net";

/* 响应体硬上限：超过即失败,绝不静默截断 */
#define NET_HTTP_RESPONSE_MAX (64 * 1024)

/* HTTP 回调上下文：可增长 body + Set-Cookie */
typedef struct {
    char  *data;
    int    len;
    int    cap;
    bool   overflow;
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
        if (rb == NULL || rb->data == NULL || evt->data_len <= 0)
            break;

        int need = rb->len + evt->data_len;
        if (need > rb->cap) {
            if (need > NET_HTTP_RESPONSE_MAX) {
                rb->overflow = true;
                break;
            }
            int new_cap = rb->cap;
            while (new_cap < need)
                new_cap = (new_cap > NET_HTTP_RESPONSE_MAX / 2)
                              ? NET_HTTP_RESPONSE_MAX
                              : new_cap * 2;
            char *grown = realloc(rb->data, (size_t)new_cap + 1);
            if (grown == NULL) {
                rb->overflow = true;
                break;
            }
            rb->data = grown;
            rb->cap = new_cap;
        }

        memcpy(rb->data + rb->len, evt->data, evt->data_len);
        rb->len += evt->data_len;
        rb->data[rb->len] = '\0';
        break;
    }

    default:
        break;
    }
    return ESP_OK;
}

static http_resp_buf_t *resp_buf_new(void)
{
    http_resp_buf_t *rb = calloc(1, sizeof *rb);
    if (rb == NULL)
        return NULL;
    rb->data = malloc(NET_HTTP_OUTPUT_MAX + 1);
    if (rb->data == NULL) {
        free(rb);
        return NULL;
    }
    rb->cap = NET_HTTP_OUTPUT_MAX;
    rb->data[0] = '\0';
    return rb;
}

static void resp_buf_free(http_resp_buf_t *rb)
{
    if (rb == NULL)
        return;
    free(rb->data);
    free(rb);
}

/* 统一 GET/POST：header_key 为空表示不带认证头,body 非空则 POST JSON。
 * 成功返回 ESP_OK 并交出 rb(调用方 resp_buf_free)；响应超限也按失败处理。 */
static esp_err_t http_request(const char *url, const char *header_key,
                              const char *header_val, const char *body,
                              http_resp_buf_t **out, int *status_out)
{
    http_resp_buf_t *rb = resp_buf_new();
    if (rb == NULL)
        return ESP_ERR_NO_MEM;

    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 8000,
        .event_handler = http_event_handler,
        .user_data = rb,
        .buffer_size = NET_HTTP_OUTPUT_MAX,
    };

    esp_http_client_handle_t http = esp_http_client_init(&cfg);
    if (http == NULL) {
        ESP_LOGE(TAG, "esp_http_client_init failed: %s", url);
        resp_buf_free(rb);
        return ESP_FAIL;
    }

    if (header_key != NULL && header_val != NULL)
        esp_http_client_set_header(http, header_key, header_val);

    if (body != NULL) {
        esp_http_client_set_method(http, HTTP_METHOD_POST);
        esp_http_client_set_header(http, "Content-Type", "application/json");
        esp_http_client_set_post_field(http, body, (int)strlen(body));
    }

    const esp_err_t err = esp_http_client_perform(http);
    *status_out = esp_http_client_get_status_code(http);
    esp_http_client_cleanup(http);

    if (err != ESP_OK) {
        resp_buf_free(rb);
        return err;
    }
    if (rb->overflow) {
        ESP_LOGE(TAG, "HTTP response exceeded %d bytes: %s",
                 NET_HTTP_RESPONSE_MAX, url);
        resp_buf_free(rb);
        return ESP_ERR_INVALID_SIZE;
    }

    *out = rb;
    return ESP_OK;
}

/* 设备 ID + 引导密钥换 Token：先试 Set-Cookie,再回退 body */
bool authbydeviceid(void)
{
    ESP_LOGI(TAG, "Starting device authentication...");

    /* NVS 已有 Token 就直接复用：省掉一次 TLS 握手和一份 HTTP 堆；
     * Token 失效时 MQTT CONNACK 会拒绝，再由恢复流程走 full re-auth。 */
    char *cached = token_load();
    if (cached != NULL && strlen(cached) > 0) {
        free(cached);
        ESP_LOGI(TAG, "Using cached device token, skip token request");
        return true;
    }
    free(cached);

    http_resp_buf_t *rb = NULL;
    int httpCode = 0;
    const esp_err_t err = http_request(TOKEN_URL, "X-Device-Secret",
                                       DEVICE_SECRET, NULL, &rb, &httpCode);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Token request failed: %s", esp_err_to_name(err));
        return false;
    }

    bool authSuccess = false;

    ESP_LOGD(TAG, "HTTP status: %d", httpCode);

    /* HTTP 200 + code==200 才算成功 */
    if (httpCode == 200 && parse_api_code(rb->data) == 200) {
        ESP_LOGI(TAG, "Login request successful");

        char *new_token = NULL;

        if (rb->set_cookie[0] != '\0')
            new_token = extractTokenFromHeader(rb->set_cookie);

        if (new_token == NULL && rb->len > 0)
            new_token = extractTokenFromBody(rb->data);

        if (new_token != NULL) {
            char *old_token = token_load();

            if (old_token != NULL && strcmp(old_token, new_token) == 0) {
                ESP_LOGI(TAG, "Token unchanged, skip saving");
            } else {
                token_save(new_token);
                ESP_LOGI(TAG, "Token updated");
            }

            free(old_token);
            free(new_token);
            authSuccess = true;
        } else {
            ESP_LOGE(TAG, "Failed to extract token from response");
        }
    } else {
        ESP_LOGE(TAG, "Authentication failed: http=%d body=%.120s",
                 httpCode, rb->data);
    }

    resp_buf_free(rb);

    if (!authSuccess)
        ESP_LOGE(TAG, "Authentication failed: cannot retrieve token");
    return authSuccess;
}

/* 拉取即消费,必须按 id 升序入队,否则会丢离线命令 */
bool syncPendingCommands(void)
{
    char *token = token_load();
    if (token == NULL || strlen(token) == 0) {
        free(token);
        return false;
    }

    http_resp_buf_t *rb = NULL;
    int httpCode = 0;
    const esp_err_t err = http_request(DATA_PULL_URL, "X-Device-Token", token,
                                       NULL, &rb, &httpCode);
    free(token);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Sync HTTP request failed: %s", esp_err_to_name(err));
        return false;
    }

    /* HTTP 200 只代表到达 */
    int apiCode = parse_api_code(rb->data);
    if (httpCode != 200 || apiCode != 200) {
        ESP_LOGW(TAG, "Sync: http=%d code=%d body=%.160s", httpCode, apiCode,
                 rb->data);
        if (apiCode == 401) {
            ESP_LOGW(TAG, "Sync: device token invalid/expired, clearing local token");
            token_clear();
        }
        resp_buf_free(rb);
        return false;
    }

    ESP_LOGD(TAG, "Sync response: %s", rb->data);

    cJSON *root = cJSON_Parse(rb->data);
    resp_buf_free(rb);

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

    if (g_commandQueue == NULL) {
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

        /* 队列满则丢弃剩余,避免死锁 */
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
