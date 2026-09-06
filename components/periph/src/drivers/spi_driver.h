#ifndef _SPI_DRIVER_H_
#define _SPI_DRIVER_H_

#include "periph.h"

/* --------------------------------------------------------------------------
 * SPI transport — SPI 主机写（DAC/移位寄存器/寄存器写等；写类走控制器，
 * 读类传感器未来走 sensor 侧采集器，本驱动不提供读回）。
 *
 * Device config:
 *   {"transport":"spi","clk":6,"mosi":7,"miso":-1,"cs":10,
 *    "freq_hz":1000000,"mode":0}
 *     clk / mosi / cs  必填
 *     miso             可选（写类可 -1/缺省）
 *     freq_hz          总线时钟（默认 1MHz）
 *     mode             0-3（默认 0）
 *   同一 host 的总线引脚（clk/mosi/miso）由首个实例初始化，后续实例复用
 *   并要求一致；每设备独立 CS。
 *
 * Control command (value 原语，hex 串或字节数组)：
 *   {"action":"dac1","value":{"tx":"A5 3C FF"}}
 *   {"action":"dac1","value":{"tx":[165,60,255]}}
 * -------------------------------------------------------------------------- */

extern periph_driver_t spi_driver;

#endif /* _SPI_DRIVER_H_ */
