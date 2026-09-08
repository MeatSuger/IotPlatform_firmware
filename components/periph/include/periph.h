#ifndef _PERIPH_H_
#define _PERIPH_H_

#include <stdbool.h>

/* --------------------------------------------------------------------------
 * Peripheral bus — 声明式外设框架（Linux device/driver 模型）。
 *
 * 设备类型（transport）不由后端 driver 字段决定 —— 后端 driver 枚举固定为
 * led/servo/speaker（仅占位透传），固件唯一依据是执行器定义 config 中的
 * "transport" 字段（gpio / pwm / spi / led_strip / ...），后续新传输按注册表
 * 扩展，后端与云端无需改动。本层只做“解析配置 → 实例化硬件 → 命令原语执行”，
 * 不做器件语义换算（角度/颜色等由云端换算成原语后下发）。
 *
 *   Linux concept          This firmware
 *   ---------------------------------------------------------------
 *   struct device_driver   periph_driver_t  (name = transport)
 *   struct device          periph_device_t  (one hardware instance)
 *   struct file_operations periph_ops_t     (command == ioctl)
 *   device tree / DT       DeviceConfig.payload.actuators (云端配置快照)
 *   of_platform_populate   periph_apply_config()
 *
 * 执行器定义（唯一真源 = 云端 config.actuators，与后端 Actuator DTO 对齐）：
 *
 *   {"id":"fan1","driver":"servo",            // driver 占位，忽略
 *    "enabled":true,
 *    "config":{"transport":"pwm","pin":18,"freq_hz":25000}}
 *
 * 控制命令（type=control payload）：
 *   {"action":"fan1","value":{...}}   — action = 设备名 → ops->command
 *   命令 value 为传输原语（level/duty/pulse_us/tx...），见 docs/mqtt-api.md。
 * -------------------------------------------------------------------------- */

typedef struct cJSON cJSON;
typedef struct periph_device periph_device_t;

/* 设备名长度上限（缓冲含终止符）；id 由云端 actuator 标识符约束（≤11） */
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
 * periph_driver_t — a transport driver (Linux device_driver analog).
 * -------------------------------------------------------------------------- */
typedef struct periph_driver {
    const char *name;      /* transport name; config "transport" field */
    const periph_ops_t *ops;   /* control operations (may be NULL) */
    /* Parse cfg (config 对象，含 transport 字段) and initialize hardware.
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
    char name[PERIPH_NAME_MAX];       /* device name (= 控制命令 action) */
    const periph_driver_t *driver;    /* transport bound at probe time */
    void *drvdata;                    /* driver-private data */
    struct periph_device *next;       /* bus list linkage */
};

/* --------------------------------------------------------------------------
 * Bus API
 * -------------------------------------------------------------------------- */

/* Initialize the bus and register the built-in transports
 * (gpio / pwm / spi / led_strip). */
void periph_bus_init(void);

/* Register a transport driver. Returns 0 on success, -1 if name exists. */
int periph_driver_register(periph_driver_t *drv);

/* Find a device by name, or NULL. */
periph_device_t *periph_device_find(const char *name);

/* Create (or reconfigure) a device from an actuator config object:
 * transport name comes from cfg->"transport" (driver 字段为后端占位，忽略)。
 * Returns true on success. */
bool periph_device_add(const char *name, const char *transport,
                       const cJSON *cfg);

/* Destroy a device: remove() 并摘链。Returns true on success. */
bool periph_device_remove(const char *name);

/* 按云端执行器定义数组 diff 应用（配置快照的 "actuators" 分区）。
 * 每条定义 {"id","driver"(忽略),"config":{"transport":...},"enabled"}：
 *   - enabled==false / config 非对象 / 无 transport：从总线移除；
 *   - 不存在/参数变化：periph_device_add（新增或重配置，复用 probe）；
 * 全部定义处理完后，移除不在期望列表中的现存设备（单向收敛到期望态）。 */
bool periph_apply_config(const cJSON *actuators);

/* Dispatch a control command payload: "action" is a device name routed
 * to the bound transport's ops->command(). Returns true on success. */
bool periph_dispatch(const cJSON *pl);

/* --------------------------------------------------------------------------
 * Pin arbitration — 同一物理引脚禁止被多个设备同时实例化。
 * gpio < 0 视为未占用（直接返回 true）。probe 中 claim，remove 中 release。
 * -------------------------------------------------------------------------- */
bool periph_pin_claim(int gpio, const char *who);
void periph_pin_release(int gpio);

#endif /* _PERIPH_H_ */
