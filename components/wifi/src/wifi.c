#include "wifi.h"

#include "common.h"
#include "esp_wpa.h" /* esp_supplicant_disable_pmk_caching */

/* --------------------------------------------------------------------------
 * WiFi event handler — signals the event group when connected / disconnected
 * -------------------------------------------------------------------------- */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        DEBUG_PRINTLN("WiFi disconnected");
        if (g_wifiEventGroup != NULL)
            xEventGroupClearBits(g_wifiEventGroup, WIFI_CONNECTED_BIT);
        esp_wifi_connect();
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        DEBUG_PRINTLN("WiFi connected, IP: " IPSTR,
                      IP2STR(&event->ip_info.ip));
        if (g_wifiEventGroup != NULL)
            xEventGroupSetBits(g_wifiEventGroup, WIFI_CONNECTED_BIT);
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
        DEBUG_PRINTLN("Failed to create WiFi event group");
        return false;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    /* Disable PMK caching BEFORE starting WiFi to avoid known IDF 6.0
     * PMKSA NULL-deref crash (espressif/esp-idf#15584, commit 437fa9a) */
    esp_supplicant_disable_pmk_caching(true);

    ESP_ERROR_CHECK(esp_wifi_start());

    /* Disable power save to avoid EAPOL rekey crash (IDF 6.0 bug) */
    esp_wifi_set_ps(WIFI_PS_NONE);

    DEBUG_PRINTLN("Connecting to Wi-Fi: %s", WIFI_SSID);

    /* Block until connected or 15 s timeout */
    EventBits_t bits = xEventGroupWaitBits(g_wifiEventGroup,
                                           WIFI_CONNECTED_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(15000));
    if (bits & WIFI_CONNECTED_BIT)
    {
        DEBUG_PRINTLN("WiFi connected successfully");
        return true;
    }
    else
    {
        DEBUG_PRINTLN("WiFi connection timeout");
        return false;
    }
}
