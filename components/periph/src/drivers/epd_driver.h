#ifndef _EPD_DRIVER_H_
#define _EPD_DRIVER_H_

#include "driver/spi_common.h" /* SPI2_HOST */
#include "periph.h"

/* epd transport — JD79665（华为 A1）3.98" 四色墨水屏，板载固定外设。
 * Device config：{"transport":"epd"}，无配置字段。
 * 引脚固定 MOSI=11 SCK=12 CS=10 DC=9 RST=8 BUSY=7，probe 占用引脚并初始化 SPI2。
 * Control command：{"action":"epd","value":{"op":"selftest"}}。
 * 位图刷屏不走 JSON（106KB 无法用命令承载），用 epd.h 的 epd_show_bitmap_stream()。
 */

#define EPD_ARRAY_LEN(a) (sizeof((a)) / sizeof((a)[0]))

/* ── 引脚（面板固定接线） ────────────────────────────────── */
#define EPD_PIN_MOSI 11
#define EPD_PIN_SCK 12
#define EPD_PIN_CS 10
#define EPD_PIN_DC 9
#define EPD_PIN_RST 8
#define EPD_PIN_BUSY 7

/* ── SPI ─────────────────────────────────────────────────── */
/* 固定走 SPI2_HOST + IOMUX 原生脚；不要改 SPI3（无 IOMUX 脚，走 GPIO matrix 会
 * 改变面板时序）。代价：与 periph 的 "spi" transport 争 SPI2_HOST。 */
#define EPD_SPI_HOST SPI2_HOST
#define EPD_SPI_HZ (10 * 1000 * 1000)
#define EPD_SPI_CHUNK 16384 /* 低于 S3 DMA 单次上限 32768 B */

/* ── 时序（ms） ──────────────────────────────────────────── */
#define EPD_RST_LOW_MS 20   /* 常规复位脉冲 */
#define EPD_RST_WAKE_MS 200 /* DSLP 深睡唤醒 */
#define EPD_RST_SETTLE_MS 50
#define EPD_BUSY_ASSERT_MS 300 /* 命令到此为止的启动延迟窗口 */
#define EPD_RESET_IDLE_MS 3000
#define EPD_IDLE_TIMEOUT_MS 15000 /* 写 GRAM 前后的空闲等待 */
#define EPD_POWERON_ASSERT_MS 5000
#define EPD_POWERON_MAX_MS 30000
#define EPD_REFRESH_ASSERT_MS 3000
#define EPD_REFRESH_MIN_MS 16000 /* 整屏波形最短时长 */
#define EPD_REFRESH_MAX_MS 40000
#define EPD_POWEROFF_MAX_MS 10000

/* ── 面板命令与参数 ──────────────────────────────────────── */
#define EPD_CMD_RESOLUTION 0x61
#define EPD_CMD_PARTIAL_WINDOW 0x83
#define EPD_CMD_POWER_ON 0x04
#define EPD_CMD_DATA_START 0x10
#define EPD_CMD_REFRESH 0x12
#define EPD_CMD_POWER_OFF 0x02
#define EPD_CMD_DEEP_SLEEP 0x07
#define EPD_DEEP_SLEEP_CODE 0xA5
#define EPD_PLL 0x08 /* 0x30：12.5 Hz */

/* 2bit 调色板；黄/红显示反了就互换这两项 */
#define EPD_COLOR_BLACK 0
#define EPD_COLOR_WHITE 1
#define EPD_COLOR_YELLOW 2
#define EPD_COLOR_RED 3

/* 0 = 4 色彩条自检，1 = Bayer 抖动灰阶带 */
#define EPD_SELFTEST_GRAY 0

extern periph_driver_t epd_driver;

#endif /* _EPD_DRIVER_H_ */
