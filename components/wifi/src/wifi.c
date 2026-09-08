#include "wifi.h"

#include "common.h"
#include "esp_wifi.h" /* WIFI_EVENT / esp_wifi_* */
#include "esp_event.h" /* esp_event_handler_instance_* */
#include "esp_netif.h" /* esp_netif_t */
#include "esp_wpa.h" /* esp_supplicant_disable_pmk_caching */
#include "esp_log.h"

static const char *TAG = "wifi";

/* --------------------------------------------------------------------------
 * WiFi event handler — signals the event group when connected / disconnected
 * -------------------------------------------------------------------------- */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        ESP_LOGW(TAG, "WiFi disconnected");
        if (g_wifiEventGroup != NULL)
        {
            xEventGroupClearBits(g_wifiEventGroup, WIFI_CONNECTED_BIT);
            /* 置位供主循环阻塞唤醒（esp_wifi_connect 自动重连在下面发起） */
            xEventGroupSetBits(g_wifiEventGroup, WIFI_DISCONNECTED_BIT);
        }
        esp_wifi_connect();
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "WiFi connected, IP: " IPSTR,
                      IP2STR(&event->ip_info.ip));
        if (g_wifiEventGroup != NULL)
        {
            xEventGroupClearBits(g_wifiEventGroup, WIFI_DISCONNECTED_BIT);
            xEventGroupSetBits(g_wifiEventGroup, WIFI_CONNECTED_BIT);
        }
    }
}

bool wifi_init_sta(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL,
        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL,
        &instance_got_ip));

    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    assert(sta_netif);

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };
    /* Create the event group BEFORE starting WiFi — the async event
     * handler (which fires on esp_wifi_start) needs it to exist. */
    g_wifiEventGroup = xEventGroupCreate();
    if (g_wifiEventGroup == NULL)
    {
        ESP_LOGE(TAG, "Failed to create WiFi event group");
        return false;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    /* Disable PMK caching BEFORE starting WiFi to avoid known IDF 6.0
     * PMKSA NULL-deref crash (espressif/esp-idf#15584) */
    esp_supplicant_disable_pmk_caching(true);

    ESP_ERROR_CHECK(esp_wifi_start());

#if PWR_SAVE_ENABLE
    /* modem-sleep：射频按 DTIM 醒来收 AP 缓存的单播，命令平均 ~50ms 到达（最坏≈一个
     * beacon 周期）。CPU light sleep 的射频协同依赖本项。
     * 历史：IDF 6.0 曾因 EAPOL rekey crash 设过 PS_NONE；现为 6.2 且 PMK caching
     * 已禁用（见上），量产前仍需 ~48h WPA2 重密钥 soak 验证 */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));
#else
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
#endif
    ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(WIFI_MAX_TX_POWER_Q4));

    ESP_LOGI(TAG, "Connecting to Wi-Fi: %s", WIFI_SSID);

    EventBits_t bits = xEventGroupWaitBits(g_wifiEventGroup,
                                           WIFI_CONNECTED_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(15000));
    if (bits & WIFI_CONNECTED_BIT)
    {
        ESP_LOGI(TAG, "WiFi connected successfully");
        return true;
    }
    else
    {
        ESP_LOGE(TAG, "WiFi connection timeout");
        return false;
    }
}
