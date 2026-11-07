/**
 * @file gpio.c
 * @brief GPIO 抽象父类实现：内存钩子默认值 + 抽象构造/析构 + 包装函数透传。
 *
 * 本文件不含任何平台相关代码（无 ESP-IDF / FreeRTOS 依赖），
 * 只做「分配 → 存 instance → 存 interfaces」以及「判空 → 透传」。
 */

#include "gpio.h"

#include <stdlib.h>

/* ------------------------------------------------------------------
 * 内存钩子默认实现：外部可在创建对象之前替换为平台专用分配器
 *     gpioMalloc = pvPortMalloc;   gpioFree = vPortFree;
 * ------------------------------------------------------------------ */
void* (*gpioMalloc)(size_t size) = malloc;
void  (*gpioFree)(void* p)       = free;

/* ------------------------------------------------------------------
 * 内部工具：句柄 / 枚举入参校验
 * ------------------------------------------------------------------ */

/* 抽象对象与具体实例都必须存在；0 表示可用，-1 表示参数非法 */
static int gpioCheckHandle(const Gpio* gpio) {
    if (gpio == NULL) {
        return GPIO_ERR_PARAM;
    }
    if (gpio->instance == NULL) {
        return GPIO_ERR_PARAM;
    }
    return GPIO_OK;
}

/* 枚举入参越界一律视为参数非法 */
static int gpioCheckEnumRange(int value, int minValue, int maxValue) {
    return (value >= minValue && value <= maxValue) ? GPIO_OK : GPIO_ERR_PARAM;
}

/* ------------------------------------------------------------------
 * 抽象构造：只做三件事 —— 分配抽象对象 / 赋 instance / 赋 interfaces
 * 不 malloc 具体类型、不解析任何业务参数。
 * ------------------------------------------------------------------ */
Gpio* NewGpio(void* instance, GpioInterface interfaces) {
    Gpio* self = NULL;

    if (NULL == instance || NULL == interfaces.deleteInstance) {
        return NULL;
    }
    if (NULL == gpioMalloc) {
        return NULL;
    }

    self = (Gpio*)gpioMalloc(sizeof(Gpio));
    if (self == NULL) {
        return NULL;
    }

    self->instance   = instance;
    self->interfaces = interfaces;

    return self;
}

/* ------------------------------------------------------------------
 * 抽象析构：先销毁具体实例，再释放抽象对象，最后置空入参指针
 * ------------------------------------------------------------------ */
void DeleteGpio(Gpio** pGpio) {
    Gpio* self = NULL;

    if (pGpio == NULL) {
        return;
    }

    self = *pGpio;
    if (self == NULL) {
        return;
    }

    if (self->interfaces.deleteInstance != NULL) {
        self->interfaces.deleteInstance(self->instance);
    }

    self->instance                  = NULL;
    self->interfaces.deleteInstance = NULL;

    if (gpioFree != NULL) {
        gpioFree(self);
    }
    self = NULL;

    *pGpio = NULL;
}

/* ------------------------------------------------------------------
 * 包装函数：先判空 / 判非法入参，再透传虚表
 *   返回 0  ：虚表调用成功返回
 *   返回 -1 ：gpio 为空、instance 为空、输出/回调指针为空、枚举越界
 *   返回 -2 ：对应接口未实现
 * ------------------------------------------------------------------ */
int GpioInit(Gpio* gpio, uint32_t pin, GpioDirection direction, GpioPull pull) {
    int rc = gpioCheckHandle(gpio);

    if (rc != GPIO_OK) {
        return rc;
    }
    if (gpioCheckEnumRange((int)direction, (int)GpioDirInput, (int)GpioDirInputOutput) != GPIO_OK) {
        return GPIO_ERR_PARAM;
    }
    if (gpioCheckEnumRange((int)pull, (int)GpioPullNone, (int)GpioPullUpDown) != GPIO_OK) {
        return GPIO_ERR_PARAM;
    }
    if (gpio->interfaces.init == NULL) {
        return GPIO_ERR_UNSUPPORTED;
    }

    return gpio->interfaces.init(gpio->instance, pin, direction, pull);
}

int GpioDeinit(Gpio* gpio, uint32_t pin) {
    int rc = gpioCheckHandle(gpio);

    if (rc != GPIO_OK) {
        return rc;
    }
    if (gpio->interfaces.deinit == NULL) {
        return GPIO_ERR_UNSUPPORTED;
    }

    return gpio->interfaces.deinit(gpio->instance, pin);
}

int GpioSetDirection(Gpio* gpio, uint32_t pin, GpioDirection direction) {
    int rc = gpioCheckHandle(gpio);

    if (rc != GPIO_OK) {
        return rc;
    }
    if (gpioCheckEnumRange((int)direction, (int)GpioDirInput, (int)GpioDirInputOutput) != GPIO_OK) {
        return GPIO_ERR_PARAM;
    }
    if (gpio->interfaces.setDirection == NULL) {
        return GPIO_ERR_UNSUPPORTED;
    }

    return gpio->interfaces.setDirection(gpio->instance, pin, direction);
}

int GpioWrite(Gpio* gpio, uint32_t pin, GpioLevel level) {
    int rc = gpioCheckHandle(gpio);

    if (rc != GPIO_OK) {
        return rc;
    }
    if (gpioCheckEnumRange((int)level, (int)GpioLevelLow, (int)GpioLevelHigh) != GPIO_OK) {
        return GPIO_ERR_PARAM;
    }
    if (gpio->interfaces.write == NULL) {
        return GPIO_ERR_UNSUPPORTED;
    }

    return gpio->interfaces.write(gpio->instance, pin, level);
}

int GpioRead(Gpio* gpio, uint32_t pin, GpioLevel* level) {
    int rc = gpioCheckHandle(gpio);

    if (rc != GPIO_OK || level == NULL) {
        return GPIO_ERR_PARAM;
    }
    if (gpio->interfaces.read == NULL) {
        return GPIO_ERR_UNSUPPORTED;
    }

    return gpio->interfaces.read(gpio->instance, pin, level);
}

int GpioToggle(Gpio* gpio, uint32_t pin) {
    int rc = gpioCheckHandle(gpio);

    if (rc != GPIO_OK) {
        return rc;
    }
    if (gpio->interfaces.toggle == NULL) {
        return GPIO_ERR_UNSUPPORTED;
    }

    return gpio->interfaces.toggle(gpio->instance, pin);
}

int GpioAttachInterrupt(Gpio* gpio, uint32_t pin, GpioEdge edge,
                        GpioIsrCallback callback, void* context) {
    int rc = gpioCheckHandle(gpio);

    if (rc != GPIO_OK || callback == NULL) {
        return GPIO_ERR_PARAM;
    }
    if (gpioCheckEnumRange((int)edge, (int)GpioEdgeNone, (int)GpioEdgeAny) != GPIO_OK) {
        return GPIO_ERR_PARAM;
    }
    if (gpio->interfaces.attachInterrupt == NULL) {
        return GPIO_ERR_UNSUPPORTED;
    }

    return gpio->interfaces.attachInterrupt(gpio->instance, pin, edge, callback, context);
}

int GpioDetachInterrupt(Gpio* gpio, uint32_t pin) {
    int rc = gpioCheckHandle(gpio);

    if (rc != GPIO_OK) {
        return rc;
    }
    if (gpio->interfaces.detachInterrupt == NULL) {
        return GPIO_ERR_UNSUPPORTED;
    }

    return gpio->interfaces.detachInterrupt(gpio->instance, pin);
}
