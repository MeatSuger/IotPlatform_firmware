// 独立 HTTPS OTA：固定 URL，开机检查镜像头版本，有新版本才写入并重启。
// 协议见 ota.h。

#include "ota.h"

#include <string.h>

#include "config.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if !OTA_ENABLE
/* 编译期关闭：空实现，esp_https_ota 符号不再被引用，链接时自动丢弃 */
void ota_check_on_boot(void) {}
#else

static const char *TAG = "ota";

/* 用于捕获系统事件的事件处理程序 */
static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data) {
  (void)arg;
  if (event_base == ESP_HTTPS_OTA_EVENT) {
    switch (event_id) {
    case ESP_HTTPS_OTA_START:
      ESP_LOGI(TAG, "OTA started");
      break;
    case ESP_HTTPS_OTA_CONNECTED:
      ESP_LOGI(TAG, "Connected to server");
      break;
    case ESP_HTTPS_OTA_GET_IMG_DESC:
      ESP_LOGI(TAG, "Reading Image Description");
      break;
    case ESP_HTTPS_OTA_VERIFY_CHIP_ID:
      ESP_LOGI(TAG, "Verifying chip id of new image: %d",
               *(esp_chip_id_t *)event_data);
      break;
    case ESP_HTTPS_OTA_VERIFY_CHIP_REVISION:
      ESP_LOGI(TAG, "Verifying chip revision of new image: %d",
               *(uint16_t *)event_data);
      break;
    case ESP_HTTPS_OTA_DECRYPT_CB:
      ESP_LOGI(TAG, "Callback to decrypt function");
      break;
    case ESP_HTTPS_OTA_WRITE_FLASH:
      ESP_LOGD(TAG, "Writing to flash: %d written", *(int *)event_data);
      break;
    case ESP_HTTPS_OTA_UPDATE_BOOT_PARTITION:
      ESP_LOGI(TAG, "Boot partition updated. Next Partition: %d",
               *(esp_partition_subtype_t *)event_data);
      break;
    case ESP_HTTPS_OTA_FINISH:
      ESP_LOGI(TAG, "OTA finish");
      break;
    case ESP_HTTPS_OTA_ABORT:
      ESP_LOGI(TAG, "OTA abort");
      break;
    }
  }
}

#define OTA_TASK_STACK 6144
#define OTA_TASK_PRIO 5

/* app_main 任务栈只有 3.5KB,放不下 TLS；升级逻辑在独立任务里跑 */
static TaskHandle_t s_caller;

/* 返回 true 表示新镜像已就绪、应重启；false 表示跳过或失败 */
static bool ota_download(void) {
  esp_http_client_config_t http = {
      .url = OTA_FW_URL,
      .crt_bundle_attach = esp_crt_bundle_attach,
      .timeout_ms = 5000,
      .keep_alive_enable = true,
      .buffer_size = 2048,
  };
  const esp_https_ota_config_t cfg = {.http_config = &http};
  esp_https_ota_handle_t handle = NULL;

  esp_err_t err = esp_https_ota_begin(&cfg, &handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "连接固件地址失败: %s", esp_err_to_name(err));
    return false;
  }

  const esp_app_desc_t *running = esp_app_get_description();
  esp_app_desc_t incoming;
  err = esp_https_ota_get_img_desc(handle, &incoming);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "读取镜像描述失败: %s", esp_err_to_name(err));
    esp_https_ota_abort(handle);
    return false;
  }

  /* 防止地址配错时把别的项目镜像刷进来 */
  if (strcmp(incoming.project_name, running->project_name) != 0) {
    ESP_LOGE(TAG, "镜像项目名不符: %s != %s", incoming.project_name,
             running->project_name);
    esp_https_ota_abort(handle);
    return false;
  }
  if (strcmp(incoming.version, running->version) == 0) {
    ESP_LOGI(TAG, "已是最新版本 %s", running->version);
    esp_https_ota_abort(handle);
    return false;
  }

  ESP_LOGI(TAG, "发现新固件 %s -> %s,开始下载", running->version,
           incoming.version);
  while ((err = esp_https_ota_perform(handle)) ==
         ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
    taskYIELD(); /* esp_https_ota_perform 内部会阻塞等待网络 */
  }

  if (err != ESP_OK) {
    ESP_LOGE(TAG, "下载失败: %s", esp_err_to_name(err));
    esp_https_ota_abort(handle);
    return false;
  }
  if (!esp_https_ota_is_complete_data_received(handle)) {
    ESP_LOGE(TAG, "镜像不完整");
    esp_https_ota_abort(handle);
    return false;
  }

  err = esp_https_ota_finish(handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "写入启动分区失败: %s", esp_err_to_name(err));
    return false;
  }
  return true;
}

static void ota_task(void *pv) {
  (void)pv;

  if (!ota_download()) {
    esp_event_handler_unregister(ESP_HTTPS_OTA_EVENT, ESP_EVENT_ANY_ID,
                                 event_handler);
    xTaskNotifyGive(s_caller);
    vTaskDelete(NULL);
    return;
  }

  ESP_LOGI(TAG, "升级完成,重启进入新固件");
  vTaskDelay(pdMS_TO_TICKS(1000));
  esp_restart();
}

void ota_check_on_boot(void) {
  if (OTA_FW_URL[0] == '\0') {
    ESP_LOGI(TAG, "OTA_FW_URL 未配置,跳过升级检查");
    return;
  }
  if (strncmp(OTA_FW_URL, "https://", 8) != 0) {
    ESP_LOGE(TAG, "OTA_FW_URL 只支持 https");
    return;
  }

  s_caller = xTaskGetCurrentTaskHandle();
  esp_err_t err = esp_event_handler_register(
      ESP_HTTPS_OTA_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL);
  if (err != ESP_OK)
    ESP_LOGW(TAG, "注册 OTA 事件回调失败: %s", esp_err_to_name(err));

  if (xTaskCreate(ota_task, "ota", OTA_TASK_STACK, NULL, OTA_TASK_PRIO, NULL) !=
      pdPASS) {
    ESP_LOGE(TAG, "创建 OTA 任务失败");
    return;
  }

  /* 升级成功会直接重启,不会通知；失败/跳过则继续启动 */
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

#endif /* OTA_ENABLE */
