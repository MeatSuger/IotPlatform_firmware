#include "periph.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "nvs_flash.h"
#include "cJSON.h"

#include "drivers/led_driver.h"
#include "drivers/servo_driver.h"
#include "drivers/speaker_driver.h"

/* --------------------------------------------------------------------------
 * Driver registry (bus)
 * -------------------------------------------------------------------------- */
static periph_driver_t *g_drivers = NULL;
static periph_device_t *g_devices = NULL;

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
 * NVS persistence — one key per device ("dev:<name>") plus a device list
 * ("dev_list") so boot-time restore does not need NVS iteration.
 * -------------------------------------------------------------------------- */
#define PERIPH_NVS_NS       "periphs"
#define PERIPH_NVS_KEY_PRE  "dev:"
#define PERIPH_NVS_KEY_MAX  15   /* NVS key length limit */
#define PERIPH_NVS_DEVLIST  "dev_list"
#define PERIPH_NVS_LIST_MAX 512

static void nvs_key(const char *name, char *key, size_t keylen)
{
    snprintf(key, keylen, PERIPH_NVS_KEY_PRE "%s", name);
}

static bool nvs_save_device(const char *name, const char *json_str)
{
    nvs_handle_t h;
    if (nvs_open(PERIPH_NVS_NS, NVS_READWRITE, &h) != ESP_OK)
        return false;

    char key[PERIPH_NVS_KEY_MAX + 1];
    nvs_key(name, key, sizeof key);
    esp_err_t err = nvs_set_str(h, key, json_str);
    if (err == ESP_OK)
        err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

static bool nvs_load_device(const char *name, char *buf, size_t buflen)
{
    nvs_handle_t h;
    if (nvs_open(PERIPH_NVS_NS, NVS_READONLY, &h) != ESP_OK)
        return false;

    char key[PERIPH_NVS_KEY_MAX + 1];
    nvs_key(name, key, sizeof key);
    size_t len = buflen;
    esp_err_t err = nvs_get_str(h, key, buf, &len);
    nvs_close(h);
    return err == ESP_OK;
}

static bool nvs_erase_device(const char *name)
{
    nvs_handle_t h;
    if (nvs_open(PERIPH_NVS_NS, NVS_READWRITE, &h) != ESP_OK)
        return false;

    char key[PERIPH_NVS_KEY_MAX + 1];
    nvs_key(name, key, sizeof key);
    esp_err_t err = nvs_erase_key(h, key);
    if (err == ESP_OK)
        err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

/* Rebuild and persist the comma-separated device name list. */
static bool nvs_save_device_list(void)
{
    char list[PERIPH_NVS_LIST_MAX] = "";
    size_t off = 0;

    for (periph_device_t *d = g_devices; d != NULL; d = d->next)
    {
        int n = snprintf(list + off, sizeof(list) - off, "%s%s",
                         off ? "," : "", d->name);
        if (n < 0 || (size_t)n >= sizeof(list) - off)
            break;
        off += (size_t)n;
    }

    nvs_handle_t h;
    if (nvs_open(PERIPH_NVS_NS, NVS_READWRITE, &h) != ESP_OK)
        return false;

    esp_err_t err = nvs_set_str(h, PERIPH_NVS_DEVLIST, list);
    if (err == ESP_OK)
        err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

static bool nvs_load_device_list(char *buf, size_t buflen)
{
    nvs_handle_t h;
    if (nvs_open(PERIPH_NVS_NS, NVS_READONLY, &h) != ESP_OK)
        return false;

    size_t len = buflen;
    esp_err_t err = nvs_get_str(h, PERIPH_NVS_DEVLIST, buf, &len);
    nvs_close(h);
    return err == ESP_OK;
}

/* --------------------------------------------------------------------------
 * Device lifecycle
 * -------------------------------------------------------------------------- */

/* Match driver, probe and link a new device. Returns the device or NULL. */
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
        DEBUG_PRINTLN("periph: probe failed for '%s' (driver %s)",
                      name, drv->name);
        free(dev);
        return NULL;
    }

    dev->next = g_devices;
    g_devices = dev;
    return dev;
}

bool periph_device_add(const char *name, const char *driver_name,
                       const cJSON *cfg)
{
    if (name == NULL || driver_name == NULL || cfg == NULL)
        return false;

    const periph_driver_t *drv = driver_find(driver_name);
    if (drv == NULL)
    {
        DEBUG_PRINTLN("periph: unknown driver '%s'", driver_name);
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
            return false;

        char *json_str = cJSON_PrintUnformatted(cfg);
        bool ok = json_str != NULL && nvs_save_device(name, json_str);
        free(json_str);
        DEBUG_PRINTLN("periph: device '%s' reconfigured (driver %s)",
                      name, driver_name);
        return ok;
    }

    dev = device_create(name, drv, cfg);
    if (dev == NULL)
        return false;

    char *json_str = cJSON_PrintUnformatted(cfg);
    bool ok = json_str != NULL && nvs_save_device(name, json_str);
    free(json_str);
    if (ok)
        ok = nvs_save_device_list();

    DEBUG_PRINTLN("periph: device '%s' added (driver %s)",
                  name, driver_name);
    return ok;
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

    nvs_erase_device(name);
    nvs_save_device_list();
    DEBUG_PRINTLN("periph: device '%s' removed", name);
    return true;
}

void periph_devices_load(void)
{
    char list[PERIPH_NVS_LIST_MAX];
    if (!nvs_load_device_list(list, sizeof list) || list[0] == '\0')
        return;

    for (char *tok = strtok(list, ","); tok != NULL;
         tok = strtok(NULL, ","))
    {
        char cfg_buf[256];
        if (!nvs_load_device(tok, cfg_buf, sizeof cfg_buf))
            continue;

        cJSON *cfg = cJSON_Parse(cfg_buf);
        if (cfg == NULL)
            continue;

        cJSON *drv_json = cJSON_GetObjectItem(cfg, "driver");
        const periph_driver_t *drv =
            cJSON_IsString(drv_json) ? driver_find(drv_json->valuestring)
                                     : NULL;
        if (drv != NULL)
        {
            if (device_create(tok, drv, cfg) != NULL)
                DEBUG_PRINTLN("periph: restored device '%s' (driver %s)",
                              tok, drv->name);
        }
        cJSON_Delete(cfg);
    }
}

/* --------------------------------------------------------------------------
 * Command dispatch — Linux "ioctl" routing for MQTT commands.
 * -------------------------------------------------------------------------- */
bool periph_dispatch(const cJSON *pl)
{
    if (pl == NULL)
        return false;

    cJSON *action = cJSON_GetObjectItem(pl, "action");
    if (!cJSON_IsString(action))
        return false;
    const char *a = action->valuestring;

    if (strcmp(a, "config") == 0)
    {
        cJSON *device = cJSON_GetObjectItem(pl, "device");
        cJSON *driver = cJSON_GetObjectItem(pl, "driver");
        cJSON *cfg = cJSON_GetObjectItem(pl, "config");
        if (!cJSON_IsString(device) || !cJSON_IsString(driver) ||
            !cJSON_IsObject(cfg))
            return false;
        return periph_device_add(device->valuestring, driver->valuestring,
                                 cfg);
    }

    if (strcmp(a, "unconfig") == 0)
    {
        cJSON *device = cJSON_GetObjectItem(pl, "device");
        if (!cJSON_IsString(device))
            return false;
        return periph_device_remove(device->valuestring);
    }

    /* Any other action is a device name -> route to its driver. */
    periph_device_t *dev = periph_device_find(a);
    if (dev == NULL || dev->driver->ops == NULL ||
        dev->driver->ops->command == NULL)
        return false;

    return dev->driver->ops->command(dev, pl);
}

/* --------------------------------------------------------------------------
 * periph_bus_init — register the built-in drivers.
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
#if PERIPH_LED_ENABLE
    periph_driver_register(&led_driver);
#endif
#if PERIPH_SERVO_ENABLE
    periph_driver_register(&servo_driver);
#endif
#if PERIPH_SPEAKER_ENABLE
    periph_driver_register(&speaker_driver);
#endif
    DEBUG_PRINTLN("periph: bus ready (%d driver(s) registered)",
                  driver_count());
}
