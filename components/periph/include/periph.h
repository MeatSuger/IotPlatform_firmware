#ifndef _PERIPH_H_
#define _PERIPH_H_

#include <stdbool.h>

/* Peripheral bus — 声明式外设总线，借鉴 Linux device/driver 模型：“解析配置 →
 * 实例化硬件 → 执行原语”，不做语义换算（角度/颜色由云端换算成原语后下发）。
 *   device_driver=periph_driver_t(name=transport)  device=periph_device_t
 *   ioctl=ops->command  device tree=config.actuators  of_platform_populate=apply_config
 * 设备类型唯一依据 specs.transport（gpio/pwm/spi/led_strip/epd；新传输只需注册
 * 驱动，云端/后端无需改动），后端 driver 字段（led/servo/speaker）只是占位被忽略；
 * 定义变更→probe/重配置，enabled=false 或缺 transport→卸载，单向收敛到期望态。
 */

typedef struct cJSON cJSON;
typedef struct periph_device periph_device_t;

/* 设备名缓冲上限（含终止符）；id 由云端 actuator 标识符约束（≤11） */
#define PERIPH_NAME_MAX 12

/* 引脚仲裁表容量（≥ GPIO_NUM_MAX） */
#define PERIPH_PIN_TABLE 64

/* periph_ops_t — driver operations（file_operations 类比，command 即 ioctl） */
typedef struct {
    /* 处理一条控制命令：pl 为完整命令 payload（含 action/value），成功返回 true */
    bool (*command)(periph_device_t *dev, const cJSON *pl);
} periph_ops_t;

/* periph_driver_t — 一个 transport 驱动（device_driver 类比） */
typedef struct periph_driver {
    const char *name;      /* transport 名，取自 specs.transport */
    const periph_ops_t *ops;   /* 控制操作，可为 NULL */
    /* 解析 cfg（含 transport 字段）并初始化硬件，成功后把驱动私有状态写入 drvdata */
    bool (*probe)(periph_device_t *dev, const cJSON *cfg);
    /* 释放 probe 占用的硬件资源 */
    void (*remove)(periph_device_t *dev);
    struct periph_driver *next;  /* 总线链表 */
} periph_driver_t;

/* periph_device_t — 一个硬件实例（device 类比） */
struct periph_device {
    char name[PERIPH_NAME_MAX];       /* 设备名 = 控制命令 action */
    const periph_driver_t *driver;    /* probe 时绑定的 transport */
    void *drvdata;                    /* 驱动私有数据 */
    struct periph_device *next;       /* 总线链表 */
};

/* Bus API */

/* 初始化总线并注册内置 transport（gpio / pwm / spi / led_strip） */
void periph_bus_init(void);

/* 注册 transport 驱动；重名返回 -1 */
int periph_driver_register(periph_driver_t *drv);

/* 按名查找设备，无则 NULL */
periph_device_t *periph_device_find(const char *name);

/* 用执行器定义创建或重配置设备；transport 取自 cfg->"transport"
 * （driver 字段为后端占位，忽略） */
bool periph_device_add(const char *name, const char *transport,
                       const cJSON *cfg);

/* 销毁设备：remove() 并摘链 */
bool periph_device_remove(const char *name);

/* 按云端执行器定义数组（配置快照的 "actuators" 分区）diff 应用：
 *   - enabled==false / specs 非对象 / 无 transport：从总线移除；
 *   - 不存在或参数变化：periph_device_add（新增或重配置，复用 probe）；
 * 最后移除不在期望列表中的现存设备，单向收敛到期望态。 */
bool periph_apply_config(const cJSON *actuators);

/* 控制命令分发：action = 设备名（= 执行器 id，≤11 字符），路由到 ops->command；
 * value 只承载传输原语（level/duty/pulse_us/tx/rgb），语义由云端换算 */
bool periph_dispatch(const cJSON *pl);

/* 引脚仲裁：同一物理引脚禁止多设备占用；gpio < 0 视为未占用（直接返回 true）；
 * probe 中 claim，remove 中 release */
bool periph_pin_claim(int gpio, const char *who);
void periph_pin_release(int gpio);

#endif /* _PERIPH_H_ */
