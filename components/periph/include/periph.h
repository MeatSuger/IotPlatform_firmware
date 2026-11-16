#ifndef _PERIPH_H_
#define _PERIPH_H_

#include <stdbool.h>

/* 声明式外设总线：按云端配置 probe 设备，命令只接收传输原语，语义换算在云端。
 * 设备类型取自 specs.transport；新增 transport 只需注册驱动。 */

typedef struct cJSON cJSON;
typedef struct periph_device periph_device_t;

/* 设备名缓冲上限(含终止符) */
#define PERIPH_NAME_MAX 12

/* 引脚仲裁表容量 */
#define PERIPH_PIN_TABLE 64

/* 驱动操作 */
typedef struct {
    /* 处理控制命令，pl 为完整 payload */
    bool (*command)(periph_device_t *dev, const cJSON *pl);
} periph_ops_t;

/* transport driver */
typedef struct periph_driver {
    const char *name;      /* 取自 specs.transport */
    const periph_ops_t *ops;   /* 可为 NULL */
    /* 初始化硬件，私有状态写入 drvdata */
    bool (*probe)(periph_device_t *dev, const cJSON *cfg);
    /* 释放 probe 占用的资源 */
    void (*remove)(periph_device_t *dev);
    struct periph_driver *next;
} periph_driver_t;

/* 设备实例 */
struct periph_device {
    char name[PERIPH_NAME_MAX];       /* 设备名 = 控制命令 action */
    const periph_driver_t *driver;    /* probe 时绑定的 transport */
    void *drvdata;                    /* 驱动私有数据 */
    cJSON *specs;                     /* 上次成功 probe 的配置深拷贝 */
    struct periph_device *next;       /* 总线链表 */
};

/* Bus API */

/* 初始化并注册内置 transport */
void periph_bus_init(void);

/* 注册 transport 驱动；重名返回 -1 */
int periph_driver_register(periph_driver_t *drv);

/* 按名查找设备，无则 NULL */
periph_device_t *periph_device_find(const char *name);

/* 创建或重配置设备；transport 取自 cfg->"transport"(driver 字段忽略) */
bool periph_device_add(const char *name, const char *transport,
                       const cJSON *cfg);

/* 移除设备并摘链 */
bool periph_device_remove(const char *name);

/* 按 actuators 定义 diff 应用：禁用或缺 transport 的移除，其余新增/重配，
 * 最后移除不在期望列表中的设备 */
bool periph_apply_config(const cJSON *actuators);

/* 按 action 路由命令；value 只含传输原语(level/duty/pulse_us/tx/rgb) */
bool periph_dispatch(const cJSON *pl);

/* 引脚仲裁：同一引脚只允许一个设备；gpio < 0 视为未占用 */
bool periph_pin_claim(int gpio, const char *who);
void periph_pin_release(int gpio);

#endif /* _PERIPH_H_ */
