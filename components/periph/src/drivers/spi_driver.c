#include "spi_driver.h"

#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "cJSON.h"
#include "driver/spi_master.h"

#define SPI_HOST          SPI2_HOST
#define SPI_TX_MAX_BYTES  256
#define SPI_DEFAULT_FREQ  1000000

/* Driver-private data (dev->drvdata). */
typedef struct
{
    int cs;
    spi_device_handle_t handle;
} spi_dev_t;

/* 总线级共享状态：首个实例初始化，末个实例释放 */
static bool g_bus_up = false;
static int g_clk = -1, g_mosi = -1, g_miso = -1;
static int g_bus_users = 0;

static void bus_teardown(void)
{
    if (g_bus_up)
    {
        spi_bus_free(SPI_HOST);
        g_bus_up = false;
    }
    /* 总线引脚由首个实例 claim，末个实例 release */
    periph_pin_release(g_clk);
    periph_pin_release(g_mosi);
    periph_pin_release(g_miso);
    g_clk = g_mosi = g_miso = -1;
    g_bus_users = 0;
}

static bool spi_probe(periph_device_t *dev, const cJSON *cfg)
{
    cJSON *jclk = cJSON_GetObjectItem(cfg, "clk");
    cJSON *jmosi = cJSON_GetObjectItem(cfg, "mosi");
    cJSON *jcs = cJSON_GetObjectItem(cfg, "cs");
    if (!cJSON_IsNumber(jclk) || !cJSON_IsNumber(jmosi) ||
        !cJSON_IsNumber(jcs))
    {
        DEBUG_PRINTLN("spi[%s]: config 需 clk/mosi/cs", dev->name);
        return false;
    }
    int clk = jclk->valueint, mosi = jmosi->valueint, cs = jcs->valueint;
    int miso = -1;
    cJSON *jmiso = cJSON_GetObjectItem(cfg, "miso");
    if (cJSON_IsNumber(jmiso) && jmiso->valueint >= 0)
        miso = jmiso->valueint;

    int freq = SPI_DEFAULT_FREQ;
    cJSON *jfreq = cJSON_GetObjectItem(cfg, "freq_hz");
    if (cJSON_IsNumber(jfreq) && jfreq->valueint > 0)
        freq = jfreq->valueint;

    int mode = 0;
    cJSON *jmode = cJSON_GetObjectItem(cfg, "mode");
    if (cJSON_IsNumber(jmode) && jmode->valueint >= 0 &&
        jmode->valueint <= 3)
        mode = jmode->valueint;

    if (!periph_pin_claim(cs, dev->name))
        return false;

    if (!g_bus_up)
    {
        /* 首实例：占用总线引脚并初始化 bus */
        if (!periph_pin_claim(clk, dev->name) ||
            !periph_pin_claim(mosi, dev->name) ||
            (miso >= 0 && !periph_pin_claim(miso, dev->name)))
        {
            DEBUG_PRINTLN("spi[%s]: 总线引脚被占用", dev->name);
            periph_pin_release(cs);
            return false;
        }

        spi_bus_config_t bus = {
            .mosi_io_num = mosi,
            .miso_io_num = miso,
            .sclk_io_num = clk,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = SPI_TX_MAX_BYTES + 16,
        };
        if (spi_bus_initialize(SPI_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK)
        {
            DEBUG_PRINTLN("spi[%s]: spi_bus_initialize 失败", dev->name);
            periph_pin_release(clk);
            periph_pin_release(mosi);
            periph_pin_release(miso);
            periph_pin_release(cs);
            return false;
        }
        g_bus_up = true;
        g_clk = clk;
        g_mosi = mosi;
        g_miso = miso;
    }
    else if (g_clk != clk || g_mosi != mosi || g_miso != miso)
    {
        /* 总线已由其它实例按不同引脚初始化：拒绝（保持单总线拓扑简单） */
        DEBUG_PRINTLN("spi[%s]: 总线引脚与已初始化实例不一致", dev->name);
        periph_pin_release(cs);
        return false;
    }

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = freq,
        .mode = mode,
        .spics_io_num = cs,
        .queue_size = 1,
    };
    spi_device_handle_t handle = NULL;
    if (spi_bus_add_device(SPI_HOST, &devcfg, &handle) != ESP_OK)
    {
        DEBUG_PRINTLN("spi[%s]: spi_bus_add_device 失败", dev->name);
        periph_pin_release(cs);
        g_bus_users--;
        if (g_bus_users == 0)
            bus_teardown();
        return false;
    }
    g_bus_users++;

    spi_dev_t *s = calloc(1, sizeof *s);
    if (s == NULL)
    {
        spi_bus_remove_device(handle);
        periph_pin_release(cs);
        g_bus_users--;
        if (g_bus_users == 0)
            bus_teardown();
        return false;
    }
    s->cs = cs;
    s->handle = handle;
    dev->drvdata = s;

    DEBUG_PRINTLN("spi[%s] ready: cs=%d clk=%d mosi=%d freq=%d mode=%d",
                  dev->name, cs, clk, mosi, freq, mode);
    return true;
}

static void spi_remove(periph_device_t *dev)
{
    spi_dev_t *s = (spi_dev_t *)dev->drvdata;
    if (s == NULL)
        return;

    if (s->handle != NULL)
        spi_bus_remove_device(s->handle);
    periph_pin_release(s->cs);

    g_bus_users--;
    if (g_bus_users == 0)
        bus_teardown();

    free(s);
    dev->drvdata = NULL;
}

/* 解析 tx：优先数字数组 [165,60,...]；其次 hex 字符串（如 "A53CFF"，
 * 允许空格/逗号/制表符分隔，必须成对字节）。非法返回 -1。 */
static int hex_val(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static int parse_tx(const cJSON *tx, uint8_t *buf, size_t cap)
{
    if (cJSON_IsArray(tx))
    {
        size_t n = 0;
        const cJSON *e;
        cJSON_ArrayForEach(e, tx)
        {
            if (!cJSON_IsNumber(e) || n >= cap)
                return -1;
            int v = e->valueint;
            if (v < 0 || v > 255)
                return -1;
            buf[n++] = (uint8_t)v;
        }
        return (int)n;
    }

    if (cJSON_IsString(tx))
    {
        const char *p = tx->valuestring;
        size_t n = 0;
        while (*p != '\0')
        {
            while (*p == ' ' || *p == '\t' || *p == ',')
                p++;
            if (*p == '\0')
                break;
            if (n >= cap)
                return -1;
            int hi = hex_val(*p++);
            int lo = (*p != '\0') ? hex_val(*p) : -1;
            if (hi < 0 || lo < 0)
                return -1; /* 奇数长度或非法字符 */
            p++;
            buf[n++] = (uint8_t)((hi << 4) | lo);
        }
        return (int)n;
    }
    return -1;
}

static bool spi_command(periph_device_t *dev, const cJSON *pl)
{
    spi_dev_t *s = (spi_dev_t *)dev->drvdata;
    if (s == NULL || s->handle == NULL)
        return false;

    cJSON *value = cJSON_GetObjectItem(pl, "value");
    if (value == NULL || !cJSON_IsObject(value))
        return false;

    cJSON *tx = cJSON_GetObjectItem(value, "tx");
    uint8_t buf[SPI_TX_MAX_BYTES];
    int len = parse_tx(tx, buf, sizeof buf);
    if (len < 0)
    {
        DEBUG_PRINTLN("spi[%s]: value.tx 需为 hex 字符串或 0-255 数组",
                      dev->name);
        return false;
    }

    spi_transaction_t t = {
        .tx_buffer = buf,
        .length = (size_t)len * 8,
    };
    return spi_device_transmit(s->handle, &t) == ESP_OK;
}

static const periph_ops_t spi_ops = {
    .command = spi_command,
};

periph_driver_t spi_driver = {
    .name = "spi",
    .ops = &spi_ops,
    .probe = spi_probe,
    .remove = spi_remove,
};
