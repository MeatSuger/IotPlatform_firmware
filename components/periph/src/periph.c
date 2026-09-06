#include "periph.h"

#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "cJSON.h"

#include "drivers/gpio_driver.h"
#include "drivers/pwm_driver.h"
#include "drivers/spi_driver.h"
#include "drivers/led_strip_driver.h"

/* --------------------------------------------------------------------------
 * Driver registry (bus) + device list
 * -------------------------------------------------------------------------- */
static periph_driver_t *g_drivers = NULL;
static periph_device_t *g_devices = NULL;

/* 引脚占用表（仲裁），GPIO_NUM_MAX 之上封顶 64 足够 */
#define PERIPH_PIN_TABLE 64
static bool g_pin_taken[PERIPH_PIN_TABLE];

bool periph_pin_claim(int gpio, const char *who)
{
    if (gpio < 0)
        return true;
    if (gpio >= PERIPH_PIN_TABLE)
        return false;
    if (g_pin_taken[gpio])
    {
        DEBUG_PRINTLN("periph: pin %d already claimed (by %s), %s rejected",
                      gpio, who, who);
        return false;
    }
    g_pin_taken[gpio] = true;
    return true;
}

void periph_pin_release(int gpio)
{
    if (gpio >= 0 && gpio < PERIPH_PIN_TABLE)
        g_pin_taken[gpio] = false;
}

int periph_driver_register(periph_driver_t *drv)
{
    if (drv == NULL || drv->name == NULL)
        return -1;

    for (periph_driver_t *d = g_drivers; d != NULL; d = d->next)
    {
        if (strcmp(d->name, drv->name) == 0)
            return -1;
    }

    drv->next = g_drivers;
    g_drivers = drv;
    return 0;
}

static const periph_driver_t *driver_find(const char *name)
{
    for (periph_driver_t *d = g_drivers; d != NULL; d = d->next)
    {
        if (strcmp(d->name, name) == 0)
            return d;
    }
    return NULL;
}

periph_device_t *periph_device_find(const char *name)
{
    if (name == NULL)
        return NULL;

    for (periph_device_t *d = g_devices; d != NULL; d = d->next)
    {
        if (strcmp(d->name, name) == 0)
            return d;
    }
    return NULL;
}

/* --------------------------------------------------------------------------
 * Device lifecycle
 * -------------------------------------------------------------------------- */

/* Match transport, probe and link a new device. Returns the device or NULL. */
static periph_device_t *device_create(const char *name,
                                      const periph_driver_t *drv,
                                      const cJSON *cfg)
{
    periph_device_t *dev = calloc(1, sizeof *dev);
    if (dev == NULL)
        return NULL;

    strncpy(dev->name, name, PERIPH_NAME_MAX - 1);
    dev->name[PERIPH_NAME_MAX - 1] = '\0';
    dev->driver = drv;

    if (!drv->probe(dev, cfg))
    {
        DEBUG_PRINTLN("periph: probe failed for '%s' (transport %s)",
                      name, drv->name);
        free(dev);
        return NULL;
    }

    dev->next = g_devices;
    g_devices = dev;
    return dev;
}

bool periph_device_add(const char *name, const char *transport,
                       const cJSON *cfg)
{
    if (name == NULL || transport == NULL || cfg == NULL)
        return false;

    const periph_driver_t *drv = driver_find(transport);
    if (drv == NULL)
    {
        DEBUG_PRINTLN("periph: unknown transport '%s'", transport);
        return false;
    }

    /* Device exists: reconfigure = remove old binding, probe anew. */
    periph_device_t *dev = periph_device_find(name);
    if (dev != NULL)
    {
        if (dev->driver->remove != NULL)
            dev->driver->remove(dev);
        dev->driver = drv;
        dev->drvdata = NULL;

        if (!drv->probe(dev, cfg))
        {
            DEBUG_PRINTLN("periph: re-probe failed for '%s' (transport %s)",
                          name, transport);
            return false;
        }
        DEBUG_PRINTLN("periph: device '%s' reconfigured (transport %s)",
                      name, transport);
        return true;
    }

    dev = device_create(name, drv, cfg);
    if (dev == NULL)
        return false;

    DEBUG_PRINTLN("periph: device '%s' added (transport %s)",
                  name, transport);
    return true;
}

bool periph_device_remove(const char *name)
{
    periph_device_t *dev = periph_device_find(name);
    if (dev == NULL)
        return false;

    if (dev->driver->remove != NULL)
        dev->driver->remove(dev);

    /* Unlink from the bus list. */
    periph_device_t **pp = &g_devices;
    while (*pp != dev)
        pp = &(*pp)->next;
    *pp = dev->next;

    free(dev);
    DEBUG_PRINTLN("periph: device '%s' removed", name);
    return true;
}

/* --------------------------------------------------------------------------
 * 期望配置 diff 应用（DeviceConfig.payload.actuators）
 *
 * 定义格式（后端 Actuator DTO；driver 为后端枚举占位，固件忽略）：
 *   {"id":"fan1","driver":"servo","enabled":true,
 *    "config":{"transport":"pwm","pin":18,"freq_hz":25000}}
 *
 * 设备类型（transport: gpio/pwm/spi/led_strip/...）由 config.transport 决定；
 * 后续新增传输只需在固件注册表加驱动，云端/后端无需改动。
 * -------------------------------------------------------------------------- */

/* id 是否出现在期望启用列表中 */
static bool desired_active(const char *name, const cJSON *actuators)
{
    const cJSON *item;
    cJSON_ArrayForEach(item, actuators)
    {
        cJSON *id = cJSON_GetObjectItem(item, "id");
        if (!cJSON_IsString(id) || strcmp(id->valuestring, name) != 0)
            continue;

        cJSON *enabled = cJSON_GetObjectItem(item, "enabled");
        return !cJSON_IsBool(enabled) || cJSON_IsTrue(enabled);
    }
    return false;
}

bool periph_apply_config(const cJSON *actuators)
{
    if (actuators == NULL || !cJSON_IsArray(actuators))
    {
        DEBUG_PRINTLN("periph: actuators 期望列表非法（非数组），忽略");
        return false;
    }

    bool all_ok = true;
    const cJSON *item;
    cJSON_ArrayForEach(item, actuators)
    {
        cJSON *id = cJSON_GetObjectItem(item, "id");
        cJSON *cfg = cJSON_GetObjectItem(item, "config");
        cJSON *enabled = cJSON_GetObjectItem(item, "enabled");

        if (!cJSON_IsString(id) || strlen(id->valuestring) == 0 ||
            strlen(id->valuestring) > PERIPH_NAME_MAX - 1)
        {
            DEBUG_PRINTLN("periph: 跳过非法执行器定义（id 缺失或超长）");
            all_ok = false;
            continue;
        }

        const char *name = id->valuestring;

        /* disabled 或非对象定义 → 移除 */
        bool active = !cJSON_IsBool(enabled) || cJSON_IsTrue(enabled);
        if (!active || !cJSON_IsObject(cfg))
        {
            if (!periph_device_remove(name) && active)
            {
                DEBUG_PRINTLN("periph: 执行器 '%s' 定义非法，无法实例化", name);
                all_ok = false;
            }
            else
            {
                DEBUG_PRINTLN("periph: 执行器 '%s' 已从期望移除", name);
            }
            continue;
        }

        /* 设备类型 = config.transport（driver 字段为后端枚举占位，忽略） */
        cJSON *transport = cJSON_GetObjectItem(cfg, "transport");
        if (!cJSON_IsString(transport) || transport->valuestring[0] == '\0')
        {
            DEBUG_PRINTLN("periph: 执行器 '%s' 缺少 config.transport，移除",
                          name);
            periph_device_remove(name);
            all_ok = false;
            continue;
        }

        if (!periph_device_add(name, transport->valuestring, cfg))
        {
            DEBUG_PRINTLN("periph: 执行器 '%s' 实例化失败（transport %s）",
                          name, transport->valuestring);
            all_ok = false;
        }
    }

    /* 收敛：移除不在期望启用列表中的现存设备 */
    periph_device_t *d = g_devices;
    while (d != NULL)
    {
        periph_device_t *next = d->next;
        if (!desired_active(d->name, actuators))
        {
            DEBUG_PRINTLN("periph: 移除不在期望列表中的设备 '%s'",
                          d->name);
            periph_device_remove(d->name);
        }
        d = next;
    }

    return all_ok;
}

/* --------------------------------------------------------------------------
 * Command dispatch — control command routing by device name.
 * -------------------------------------------------------------------------- */
bool periph_dispatch(const cJSON *pl)
{
    if (pl == NULL)
        return false;

    cJSON *action = cJSON_GetObjectItem(pl, "action");
    if (!cJSON_IsString(action))
        return false;

    /* action = 设备名 → 路由到其 transport 的 ops->command */
    periph_device_t *dev = periph_device_find(action->valuestring);
    if (dev == NULL || dev->driver->ops == NULL ||
        dev->driver->ops->command == NULL)
        return false;

    return dev->driver->ops->command(dev, pl);
}

/* --------------------------------------------------------------------------
 * periph_bus_init — register the built-in transports.
 * -------------------------------------------------------------------------- */
static int driver_count(void)
{
    int n = 0;
    for (periph_driver_t *d = g_drivers; d != NULL; d = d->next)
        n++;
    return n;
}

void periph_bus_init(void)
{
    periph_driver_register(&gpio_driver);
    periph_driver_register(&pwm_driver);
    periph_driver_register(&spi_driver);
#if PERIPH_LED_ENABLE
    periph_driver_register(&led_strip_driver);
#endif
    DEBUG_PRINTLN("periph: bus ready (%d transport(s) registered)",
                  driver_count());
}
