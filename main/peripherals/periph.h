#ifndef _PERIPH_H_
#define _PERIPH_H_

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* --------------------------------------------------------------------------
 * Peripheral bus — modeled after the Linux kernel device/driver model.
 *
 *  Linux concept          This firmware
 *  ---------------------------------------------------------------
 *  struct device_driver   periph_driver_t  (name + probe/remove + ops)
 *  struct device          periph_device_t  (one hardware instance)
 *  struct file_operations periph_ops_t     (command == ioctl)
 *  driver_register()      periph_driver_register()
 *  device_add()           periph_device_add()  (config via MQTT)
 *  device_remove()        periph_device_remove()
 *  device tree / DT       MQTT "config" command, persisted in NVS
 *  of_platform_populate   periph_devices_load()  (restore from NVS)
 *
 * Lifecycle: a device is created from a JSON config (the "device tree
 * node"), matched to a driver by name, then probed.  probe() parses the
 * config, initializes the hardware and allocates driver-private data
 * (dev->drvdata).  remove() tears the hardware down.
 *
 * MQTT protocol (payload "action" field):
 *   {"action":"config","device":"led","driver":"led",
 *    "config":{"gpio":48,"count":1}}          — create / update device
 *   {"action":"unconfig","device":"led"}      — destroy device
 *   {"action":"led","value":{...}}            — control device (ops->command)
 * -------------------------------------------------------------------------- */

typedef struct cJSON cJSON;
typedef struct periph_device periph_device_t;

/* NVS key is "dev:<name>" and must stay <= 15 chars -> name <= 11. */
#define PERIPH_NAME_MAX 12

/* --------------------------------------------------------------------------
 * periph_ops_t — driver operations (Linux file_operations analog).
 * -------------------------------------------------------------------------- */
typedef struct {
    /* Handle a control command for this device. pl is the full command
     * payload object (contains "action", "value", ...). Return true on
     * success. */
    bool (*command)(periph_device_t *dev, const cJSON *pl);
} periph_ops_t;

/* --------------------------------------------------------------------------
 * periph_driver_t — a device driver (Linux device_driver analog).
 * -------------------------------------------------------------------------- */
typedef struct periph_driver {
    const char *name;      /* driver name; device config "driver" field */
    const periph_ops_t *ops;   /* control operations (may be NULL) */
    /* Parse cfg (NULL = defaults) and initialize hardware.
     * On success fill dev->drvdata with driver-private state. */
    bool (*probe)(periph_device_t *dev, const cJSON *cfg);
    /* Release hardware resources allocated by probe(). */
    void (*remove)(periph_device_t *dev);
    struct periph_driver *next;  /* bus list linkage */
} periph_driver_t;

/* --------------------------------------------------------------------------
 * periph_device_t — a hardware device instance (Linux device analog).
 * -------------------------------------------------------------------------- */
struct periph_device {
    char name[PERIPH_NAME_MAX];       /* device name (MQTT action, NVS key) */
    const periph_driver_t *driver;    /* driver bound at probe time */
    void *drvdata;                    /* driver-private data */
    struct periph_device *next;       /* bus list linkage */
};

/* --------------------------------------------------------------------------
 * Bus API
 * -------------------------------------------------------------------------- */

/* Initialize the bus and register the built-in drivers
 * (periph_driver_register is called for each built-in driver). */
void periph_bus_init(void);

/* Register a driver (Linux driver_register analog). Returns 0 on
 * success, -1 if name already registered. */
int periph_driver_register(periph_driver_t *drv);

/* Find a device by name, or NULL. */
periph_device_t *periph_device_find(const char *name);

/* Create (or reconfigure) a device: match driver, probe, persist config
 * to NVS. Returns true on success. */
bool periph_device_add(const char *name, const char *driver_name,
                       const cJSON *cfg);

/* Destroy a device: remove(), drop from NVS. Returns true on success. */
bool periph_device_remove(const char *name);

/* Restore all persisted devices from NVS (called once at boot). */
void periph_devices_load(void);

/* Dispatch an MQTT command payload: "config"/"unconfig" are bus-level
 * actions, any other "action" is treated as a device name and routed to
 * the bound driver's ops->command(). Returns true on success. */
bool periph_dispatch(const cJSON *pl);

#endif /* _PERIPH_H_ */
