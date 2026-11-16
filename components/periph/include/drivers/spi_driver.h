#ifndef _SPI_DRIVER_H_
#define _SPI_DRIVER_H_

#include "driver/spi_common.h" /* SPI2_HOST */
#include "periph.h"

#define SPI_TRANSPORT_HOST SPI2_HOST
#define SPI_TRANSPORT_TX_MAX 256   /* 单笔最长发送字节数 */
#define SPI_TRANSPORT_FREQ_HZ 1000000 /* freq_hz 缺省值 */

/* SPI transport — SPI 主机写（DAC / 移位寄存器 / 寄存器写等；读类传感器未来走
 * sensor 侧采集器，本驱动不提供读回）。
 * Device config：{"transport":"spi","clk":6,"mosi":7,"miso":-1,"cs":10,
 *   "freq_hz":1000000,"mode":0}；clk/mosi/cs 必填，miso 写类可 -1/缺省，
 *   freq_hz 默认 1MHz，mode 0-3 默认 0。
 * 同一 host 的总线引脚（clk/mosi/miso）由首实例初始化，后续实例复用并要求一致；
 * 每设备独立 CS。
 * Control command（value 原语：hex 串或字节数组）：
 *   {"action":"dac1","value":{"tx":"A5 3C FF"}} 或 {"tx":[165,60,255]}
 */

extern periph_driver_t spi_driver;

#endif /* _SPI_DRIVER_H_ */
