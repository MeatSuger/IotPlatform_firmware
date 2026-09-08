#include "token.h"

#include <stdlib.h> /* malloc/free */
#include <string.h>
#include "nvs.h"
#include "esp_log.h"

#define NVS_NAMESPACE "device"
#define NVS_KEY_TOKEN "auth_token"

static const char *TAG = "token";

char *token_load(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed: 0x%x", err);
        return NULL;
    }

    size_t len = 0;
    err = nvs_get_str(handle, NVS_KEY_TOKEN, NULL, &len);
    if (err != ESP_OK || len == 0) {
        nvs_close(handle);
        return NULL;
    }

    char *token = malloc(len);
    if (token == NULL) {
        nvs_close(handle);
        return NULL;
    }

    err = nvs_get_str(handle, NVS_KEY_TOKEN, token, &len);
    nvs_close(handle);

    if (err != ESP_OK) {
        free(token);
        return NULL;
    }

    ESP_LOGI(TAG, "Token loaded from NVS (%zu bytes)", len);
    return token;
}

bool token_save(const char *token)
{
    if (token == NULL)
        return false;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: 0x%x", err);
        return false;
    }

    err = nvs_set_str(handle, NVS_KEY_TOKEN, token);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_str failed: 0x%x", err);
        nvs_close(handle);
        return false;
    }

    err = nvs_commit(handle);
    nvs_close(handle);

    if (err == ESP_OK)
        ESP_LOGI(TAG, "Token saved to NVS (%zu bytes)", strlen(token));
    return (err == ESP_OK);
}

void token_clear(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        nvs_erase_key(handle, NVS_KEY_TOKEN);
        nvs_commit(handle);
        nvs_close(handle);
        ESP_LOGI(TAG, "Token cleared from NVS");
    }
}
