#ifndef _OTA_H_
#define _OTA_H_

/* 独立 OTA：开机联网后检查一次编译期固定地址(config.h 的 OTA_FW_URL)。
 * 使用 ESP-IDF 官方 esp_https_ota，不经 MQTT、不依赖云端配置。
 * 先读镜像头版本：与运行版本相同则跳过；不同则整包下载写入备用分区并重启。
 * 只接受 https；OTA_FW_URL 为空串时关闭。 */
void ota_check_on_boot(void);

#endif /* _OTA_H_ */
