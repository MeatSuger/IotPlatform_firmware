#include "wifi.h"

#include <string.h>

#include "common.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_smartconfig.h"
#include "esp_wifi.h"
#include "esp_wpa.h" /* esp_supplicant_disable_pmk_caching */
#include "nvs.h"

static const char *TAG = "wifi";

/* CONNECTING：连接已存网络；PROVISIONING：SmartConfig 监听；UP：已拿到 IP */
typedef enum {
  WIFI_STA_CONNECTING = 0,
  WIFI_STA_PROVISIONING,
  WIFI_STA_UP,
} wifi_sta_state_t;

static volatile wifi_sta_state_t s_sta_state = WIFI_STA_CONNECTING;
static bool s_prov_mode = false;
static int s_connect_failures = 0;

/* 配网凭据连不上(密码错/AP 不在)时重新广播，避免永久卡死 */
#define WIFI_PROV_CONNECT_ATTEMPTS 3

/* 拿到 IP 后才置位，避免把连不上的网络固化 */
static bool prov_flag_read(void) {
  nvs_handle_t h;
  uint8_t v = 0;
  if (nvs_open(WIFI_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
    nvs_get_u8(h, WIFI_NVS_KEY_PROVISIONED, &v);
    nvs_close(h);
  }
  return v != 0;
}

static void prov_flag_write(void) {
  nvs_handle_t h;
  if (nvs_open(WIFI_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
    nvs_set_u8(h, WIFI_NVS_KEY_PROVISIONED, 1);
    nvs_commit(h);
    nvs_close(h);
  } else {
    ESP_LOGW(TAG, "Failed to persist provisioning flag");
  }
}

/* ESP-Touch + AirKiss */
static void start_smartconfig(void) {
  esp_err_t e = esp_smartconfig_set_type(SC_TYPE_ESPTOUCH_AIRKISS);
  if (e != ESP_OK) {
    ESP_LOGE(TAG, "Set SmartConfig type failed: %s", esp_err_to_name(e));
    return;
  }
  smartconfig_start_config_t cfg = SMARTCONFIG_START_CONFIG_DEFAULT();
  e = esp_smartconfig_start(&cfg);
  if (e != ESP_OK)
    ESP_LOGE(TAG, "SmartConfig start failed: %s", esp_err_to_name(e));
  else
    ESP_LOGI(TAG, "SmartConfig listening (ESP-Touch / AirKiss)");
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
  (void)arg;
  if (event_base == WIFI_EVENT) {
    switch (event_id) {
    case WIFI_EVENT_STA_START:
      if (s_sta_state != WIFI_STA_PROVISIONING)
        esp_wifi_connect();
      break;

    case WIFI_EVENT_STA_DISCONNECTED:
      ESP_LOGW(TAG, "WiFi disconnected");
      if (g_wifiEventGroup != NULL) {
        xEventGroupClearBits(g_wifiEventGroup, WIFI_CONNECTED_BIT);
        xEventGroupSetBits(g_wifiEventGroup, WIFI_DISCONNECTED_BIT);
      }
      if (s_sta_state == WIFI_STA_CONNECTING && s_prov_mode &&
          ++s_connect_failures >= WIFI_PROV_CONNECT_ATTEMPTS) {
        s_connect_failures = 0;
        s_sta_state = WIFI_STA_PROVISIONING;
        ESP_LOGW(TAG, "WiFi connect failed %d times, restarting SmartConfig",
                 WIFI_PROV_CONNECT_ATTEMPTS);
        esp_smartconfig_stop();
        start_smartconfig();
        break;
      }
      if (s_sta_state != WIFI_STA_PROVISIONING)
        esp_wifi_connect(); /* 配网监听态不发起连接 */
      break;

    default:
      break;
    }
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(TAG, "WiFi connected, IP: " IPSTR, IP2STR(&event->ip_info.ip));
    s_sta_state = WIFI_STA_UP;
    s_connect_failures = 0;
    s_prov_mode = false;
    if (g_wifiEventGroup != NULL) {
      xEventGroupClearBits(g_wifiEventGroup, WIFI_DISCONNECTED_BIT);
      xEventGroupSetBits(g_wifiEventGroup, WIFI_CONNECTED_BIT);
    }
  }
}

static void smartconfig_event_handler(void *arg, esp_event_base_t event_base,
                                      int32_t event_id, void *event_data) {
  (void)arg;
  switch (event_id) {
  case SC_EVENT_GOT_SSID_PSWD:
    if (s_sta_state != WIFI_STA_PROVISIONING)
      break; /* 已停止配网的迟到事件，忽略 */
    {
      const smartconfig_event_got_ssid_pswd_t *ev =
          (const smartconfig_event_got_ssid_pswd_t *)event_data;

      wifi_config_t wc = {0};
      memcpy(wc.sta.ssid, ev->ssid,
             sizeof(wc.sta.ssid)); /* ev 缓冲已 NUL 终止 */
      memcpy(wc.sta.password, ev->password, sizeof(wc.sta.password));

      s_sta_state = WIFI_STA_CONNECTING;
      s_connect_failures = 0;
      esp_err_t e =
          esp_wifi_set_config(WIFI_IF_STA, &wc); /* 持久化到 WiFi NVS */
      ESP_LOGI(TAG, "SmartConfig got credentials, connecting to %s ... %s",
               wc.sta.ssid, e == ESP_OK ? "ok" : esp_err_to_name(e));
      if (e == ESP_OK)
        esp_wifi_connect();
    }
    break;

  case SC_EVENT_SCAN_DONE:
    ESP_LOGI(TAG, "SmartConfig: channel scan done");
    break;

  case SC_EVENT_SEND_ACK_DONE:
    ESP_LOGI(TAG, "SmartConfig: ACK sent to phone");
    esp_smartconfig_stop(); /* 必须先停，释放 SmartConfig 占用 */
    break;

  default:
    break;
  }
}

bool wifi_init_sta(void) {
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  esp_event_handler_instance_t instance_wifi;
  esp_event_handler_instance_t instance_ip;
  esp_event_handler_instance_t instance_sc;
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_wifi));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_ip));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      SC_EVENT, ESP_EVENT_ANY_ID, &smartconfig_event_handler, NULL,
      &instance_sc));

  esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
  assert(sta_netif);

  /* 事件回调依赖事件组，必须在 esp_wifi_start 前创建 */
  g_wifiEventGroup = xEventGroupCreate();
  if (g_wifiEventGroup == NULL) {
    ESP_LOGE(TAG, "Failed to create WiFi event group");
    return false;
  }

  /* 无已配网标记则进配网模式 */
  s_prov_mode = !prov_flag_read();
  s_sta_state = s_prov_mode ? WIFI_STA_PROVISIONING : WIFI_STA_CONNECTING;

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

  /* 规避 IDF 6.0 PMKSA NULL-deref 崩溃(espressif/esp-idf#15584) */
  esp_supplicant_disable_pmk_caching(true);

  ESP_ERROR_CHECK(esp_wifi_start());

  if (s_prov_mode) {
    /* 配网时关 modem-sleep，否则 promiscuous 收包漏帧 */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    start_smartconfig();
  } else {
    /* 有标记但无 STA profile(异常清库)→ 转配网 */
    wifi_config_t cur;
    bool profile_ok = (esp_wifi_get_config(WIFI_IF_STA, &cur) == ESP_OK &&
                       cur.sta.ssid[0] != '\0');
    if (!profile_ok) {
      ESP_LOGW(TAG,
               "No saved STA profile, switching to SmartConfig provisioning");
      s_prov_mode = true;
      s_sta_state = WIFI_STA_PROVISIONING;
      ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
      esp_wifi_disconnect(); /* 取消空配置连接，避免与监听竞态 */
      start_smartconfig();
    }
  }

  ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(WIFI_MAX_TX_POWER_Q4));

  bool ok = false;
  if (s_prov_mode) {
    ESP_LOGI(TAG,
             "SmartConfig provisioning mode — phone App: broadcast your WiFi");
    /* 阻塞直到手机广播凭据并连上 */
    EventBits_t bits = xEventGroupWaitBits(g_wifiEventGroup, WIFI_CONNECTED_BIT,
                                           pdFALSE, pdFALSE, portMAX_DELAY);
    ok = (bits & WIFI_CONNECTED_BIT) != 0;
    if (ok)
      prov_flag_write();
  } else {
    EventBits_t bits =
        xEventGroupWaitBits(g_wifiEventGroup, WIFI_CONNECTED_BIT, pdFALSE,
                            pdFALSE, pdMS_TO_TICKS(15000));
    ok = (bits & WIFI_CONNECTED_BIT) != 0;
  }

  if (ok) {
#if PWR_SAVE_ENABLE
    /* modem-sleep：射频按 DTIM 醒来收缓存单播 */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));
#else
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
#endif
    ESP_LOGI(TAG, "WiFi connected successfully");
  } else {
    ESP_LOGE(TAG, "WiFi connection timeout");
  }
  return ok;
}