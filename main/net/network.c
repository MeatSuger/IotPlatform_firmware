#include "network.h"
#include "sensor.h"
#include "token.h"

#include <strings.h>
#include "esp_http_client.h"
#include "esp_crt_bundle.h" /* esp_crt_bundle_attach (IDF bundled CA bundle) */
#include "cJSON.h"
#include "esp_log.h"

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
 * authbydeviceid — authenticate to the API server using device credentials.
 *
 * Tries the Set-Cookie header first, then falls back to the JSON body.
 *
 * Returns: true if a token was obtained.
 * -------------------------------------------------------------------------- */
bool authbydeviceid(void)
{

    DEBUG_PRINTLN("Starting user authentication...");

    /* Allocate everything on heap — main task stack is only 3584 bytes */
    char *buf_data = malloc(MAX_HTTP_OUTPUT_BUFFER);
    http_resp_buf_t *rbuf = malloc(sizeof(http_resp_buf_t));
    esp_http_client_config_t *cfg = malloc(sizeof(esp_http_client_config_t));
    if (buf_data == NULL || rbuf == NULL || cfg == NULL) {
        DEBUG_PRINTLN("OOM: cannot allocate HTTP buffers");
        free(buf_data); free(rbuf); free(cfg);
        return false;
    }
    *rbuf = (http_resp_buf_t){ .data = buf_data, .len = 0,
                                .cap = MAX_HTTP_OUTPUT_BUFFER };
    make_http_config(cfg, TOKEN_URL, rbuf);
    DEBUG_PRINTLN("HTTP URL = [%s]", cfg->url);
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

    DEBUG_PRINT("HTTP status: %d\n", httpCode);

    if (httpCode == 200) {
        DEBUG_PRINTLN("Login request successful");

        char *new_token = NULL;

        /* 先尝试从 Set-Cookie 提取 */
        if (strlen(rbuf->set_cookie) > 0) {
            new_token = extractTokenFromHeader(rbuf->set_cookie);
        }

        /* 如果未提取到，尝试从 JSON body 提取 */
        if (new_token == NULL && rbuf->len > 0) {
            new_token = extractTokenFromBody(rbuf->data);
        }

        if (new_token != NULL) {
            /* 加载旧 token（可能为 NULL） */
            char *old_token = token_load();

            if (old_token != NULL && strcmp(old_token, new_token) == 0) {
                DEBUG_PRINTLN("Token unchanged, skip saving");
            } else {
                token_save(new_token);
                DEBUG_PRINTLN("Token updated (new token differs from old or old was NULL)");
            }

            free(old_token);   // token_load 返回的是 malloc 的副本
            free(new_token);
            authSuccess = true;
        } else {
            DEBUG_PRINTLN("Failed to extract token from response");
            authSuccess = false;
        }
    }

    esp_http_client_cleanup(http);

    if (authSuccess) {
        char *tok = token_load();
        DEBUG_PRINT("Auth token: %s\n", tok ? tok : "(null)");
        free(tok);
    }
    else
        DEBUG_PRINTLN("Authentication failed: cannot retrieve token");

    free(cfg); free(rbuf); free(buf_data);
    return authSuccess;
}

/* --------------------------------------------------------------------------
 * sendSensorData — POST sensor JSON to the data endpoint.
 *
 * Returns: true on HTTP 200/201, false on failure or when unauthenticated.
 * -------------------------------------------------------------------------- */
bool sendSensorData(void)
{
    char *token = token_load();
    if (token == NULL || strlen(token) == 0) {
        free(token);
        DEBUG_PRINTLN("Not authenticated, cannot send data");
        return false;
    }

    char *buf_data = malloc(MAX_HTTP_OUTPUT_BUFFER);
    if (buf_data == NULL) {
        free(token);
        DEBUG_PRINTLN("OOM: cannot allocate HTTP response buffer");
        return false;
    }
    http_resp_buf_t rbuf = { .data = buf_data, .len = 0,
                              .cap = MAX_HTTP_OUTPUT_BUFFER };

    esp_http_client_config_t cfg;
    make_http_config(&cfg, DATA_URL, &rbuf);
    cfg.method = HTTP_METHOD_POST;
    esp_http_client_handle_t http = esp_http_client_init(&cfg);

    esp_http_client_set_header(http, "Content-Type", "application/json");
    esp_http_client_set_header(http, "X-Device-Token", token);

    char *jsonData = createSensorData();
    if (jsonData == NULL) {
        esp_http_client_cleanup(http);
        free(buf_data);
        free(token);
        return false;
    }

    esp_http_client_set_post_field(http, jsonData, strlen(jsonData));

    esp_err_t err = esp_http_client_perform(http);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP perform failed: 0x%x", err);
        free(jsonData);
        esp_http_client_cleanup(http);
        free(buf_data);
        free(token);
        return false;
    }
    int httpCode = esp_http_client_get_status_code(http);
    bool success = false;

    if (httpCode > 0) {
        DEBUG_PRINT("HTTP response code: %d\n", httpCode);
        DEBUG_PRINT("Server response: %s\n", rbuf.data);

        if (httpCode == 200 || httpCode == 201) {
            success = true;
        } else {
            DEBUG_PRINTLN("Data send failed");
            if (httpCode == 401) {
                DEBUG_PRINTLN("Token expired");
                token_clear();
            }
        }
    } else {
        DEBUG_PRINT("Request failed, error: %d\n", httpCode);
        char err_buf[64];
        esp_err_to_name_r(err, err_buf, sizeof(err_buf));
        DEBUG_PRINT("Error message: %s\n", err_buf);
    }

    free(jsonData);
    esp_http_client_cleanup(http);
    free(buf_data);
    free(token);
    return success;
}

/* --------------------------------------------------------------------------
 * syncPendingCommands — Fetch pending commands on boot, enqueue the latest.
 *
 * Polls the device command endpoint, locates the entry with the highest id,
 * serializes it and pushes it into g_commandQueue.
 *
 * Returns: true if a command was found and enqueued.
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
        DEBUG_PRINTLN("Sync: OOM");
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

    if (httpCode != 200) {
        DEBUG_PRINT("Sync: HTTP error %d\n", httpCode);
        esp_http_client_cleanup(http);
        free(cfg); free(rbuf); free(buf_data); free(token);
        return false;
    }

    esp_http_client_cleanup(http);

    DEBUG_PRINT("Sync response: %s\n", rbuf->data);

    /* Parse JSON */
    cJSON *root = cJSON_Parse(rbuf->data);
    free(cfg); free(rbuf); free(buf_data);

    if (root == NULL) {
        DEBUG_PRINTLN("Sync: JSON parse error");
        free(token);
        return false;
    }

    cJSON *arr = cJSON_GetObjectItem(root, "data");
    if (arr == NULL || !cJSON_IsArray(arr)) {
        DEBUG_PRINTLN("Sync: no data array");
        cJSON_Delete(root);
        free(token);
        return false;
    }

    /* Find the entry with the highest id */
    cJSON *item;
    cJSON *latest = NULL;
    int maxId = -1;
    cJSON_ArrayForEach(item, arr) {
        cJSON *cid = cJSON_GetObjectItem(item, "id");
        if (cid != NULL && cJSON_IsNumber(cid) && cid->valueint > maxId) {
            maxId = cid->valueint;
            latest = item;
        }
    }

    if (latest == NULL) {
        DEBUG_PRINTLN("Sync: no pending commands");
        cJSON_Delete(root);
        free(token);
        return false;
    }

    /* Serialize and enqueue */
    char *cmdStr = cJSON_PrintUnformatted(latest);
    CommandMsg cmdMsg;
    cmdMsg.payload = cmdStr;
    cmdMsg.length = strlen(cmdStr);

    /* Use a timeout to avoid permanent deadlock if the queue is
     * unexpectedly full (should never happen at boot, but defensive). */
    if (xQueueSend(g_commandQueue, &cmdMsg, pdMS_TO_TICKS(5000)) != pdPASS)
    {
        DEBUG_PRINTLN("Sync: queue full, dropping command");
        free(cmdStr);
    }
    else
    {
        DEBUG_PRINT("Sync: enqueued cmd id=%d\n", maxId);
        /* cmdStr is intentionally not freed here — cmdProcessTask will parse
         * and free it.  Called only once at boot so the leak is bounded. */
    }
    cJSON_Delete(root);
    free(token);
    return true;
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
        DEBUG_PRINT("JSON parse error: %s\n",
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
