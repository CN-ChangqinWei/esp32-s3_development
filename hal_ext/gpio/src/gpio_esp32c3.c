/**
 * @file gpio_esp32c3.c
 * @brief Gpio 抽象父类的 ESP32-C3 具象派生实现（ESP-IDF v5.5.x）。
 *
 * 设计要点：
 *  1. 本文件是唯一出现平台类型（esp_err_t / gpio_num_t / gpio_config_t …）的地方，
 *     对外头文件 gpio_esp32c3.h 保持「零平台依赖」。
 *  2. 全部接口函数为 static，首参恒为 void* instance；对外只暴露
 *     GpioEsp32c3Interface()（虚表）与 NewGpioEsp32c3()（具体构造）。
 *  3. 中断回调必须转一层 trampoline：IDF 的 gpio_isr_t 是
 *     void (*)(void* arg)，而用户回调是 GpioIsrCallback(void* context, uint32_t pin)，
 *     所以用「静态固定表」保存 {回调, 用户 context, pin}，
 *     把槽位地址作为 arg 传给 gpio_isr_handler_add —— ISR 内没有任何堆操作。
 */

#include "gpio_esp32c3.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_err.h"

/* ------------------------------------------------------------------
 * 平台兜底：GPIO_IS_VALID_GPIO 在不同 IDF 版本中可能来自 driver/gpio.h
 * 或 soc 头，若当前包含链没有暴露它，则按 ESP32-C3 的 0~21 自行校验。
 * ------------------------------------------------------------------ */
#ifndef GPIO_IS_VALID_GPIO
#define GPIO_IS_VALID_GPIO(pin) \
    (((int)(pin) >= 0) && ((int)(pin) < (int)GPIO_ESP32C3_VALID_PIN_COUNT))
#endif

/* ==================================================================
 * 中断转发器：静态固定表，按引脚号索引，ISR 内只读
 * ================================================================== */
typedef struct GpioEsp32c3IsrTrampoline {
    GpioIsrCallback callback;    /* 用户回调 */
    void*           userContext; /* 用户 attach 时传入的 context */
    GpioEsp32c3*    owner;       /* 所属具体实例，便于校验归属 */
    uint32_t        pin;         /* 触发引脚号，回调时透传给用户 */
    uint8_t         busy;        /* 槽位占用标志（1=已注册） */
} GpioEsp32c3IsrTrampoline;

static GpioEsp32c3IsrTrampoline sGpioEsp32c3IsrTrampolines[GPIO_ESP32C3_ISR_SLOT_COUNT];

/* ==================================================================
 * 枚举 -> 平台常量映射
 * ================================================================== */
static gpio_mode_t gpioEsp32c3ModeFromDirection(GpioDirection direction) {
    switch (direction) {
        case GpioDirInput:       return GPIO_MODE_INPUT;
        case GpioDirOutput:      return GPIO_MODE_OUTPUT;
        case GpioDirInputOutput: return GPIO_MODE_INPUT_OUTPUT;
        default:                 return GPIO_MODE_DISABLE;
    }
}

static gpio_pull_mode_t gpioEsp32c3PullModeFromPull(GpioPull pull) {
    switch (pull) {
        case GpioPullUp:     return GPIO_PULLUP_ONLY;
        case GpioPullDown:   return GPIO_PULLDOWN_ONLY;
        case GpioPullUpDown: return GPIO_PULLUP_PULLDOWN;
        case GpioPullNone:
        default:             return GPIO_FLOATING;
    }
}

static gpio_int_type_t gpioEsp32c3IntrTypeFromEdge(GpioEdge edge) {
    switch (edge) {
        case GpioEdgeRising:  return GPIO_INTR_POSEDGE;
        case GpioEdgeFalling: return GPIO_INTR_NEGEDGE;
        case GpioEdgeAny:     return GPIO_INTR_ANYEDGE;
        case GpioEdgeNone:
        default:              return GPIO_INTR_DISABLE;
    }
}

/* ==================================================================
 * 内部工具：入参校验
 * ================================================================== */

/* 引脚合法性：非法引脚 -1；没有中断槽位的引脚 -2；0 表示可用 */
static int gpioEsp32c3CheckPin(uint32_t pin) {
    if (!GPIO_IS_VALID_GPIO((int)pin)) {
        return GPIO_ERR_PARAM;
    }
    if (pin >= (uint32_t)GPIO_ESP32C3_ISR_SLOT_COUNT) {
        return GPIO_ERR_UNSUPPORTED;
    }
    return GPIO_OK;
}

static int gpioEsp32c3CheckDirection(GpioDirection direction) {
    return ((int)direction >= (int)GpioDirInput && (int)direction <= (int)GpioDirInputOutput)
               ? GPIO_OK
               : GPIO_ERR_PARAM;
}

static int gpioEsp32c3CheckPull(GpioPull pull) {
    return ((int)pull >= (int)GpioPullNone && (int)pull <= (int)GpioPullUpDown)
               ? GPIO_OK
               : GPIO_ERR_PARAM;
}

static int gpioEsp32c3CheckEdge(GpioEdge edge) {
    return ((int)edge >= (int)GpioEdgeNone && (int)edge <= (int)GpioEdgeAny)
               ? GPIO_OK
               : GPIO_ERR_PARAM;
}

static int gpioEsp32c3CheckLevel(GpioLevel level) {
    return ((int)level >= (int)GpioLevelLow && (int)level <= (int)GpioLevelHigh)
               ? GPIO_OK
               : GPIO_ERR_PARAM;
}

/* ==================================================================
 * 中断转发器：ISR 本体 + 槽位管理
 * ================================================================== */

/* 真正的 ISR 入口：只做「取快照 -> 转发用户回调」，无堆操作、无阻塞调用 */
static void IRAM_ATTR gpioEsp32c3IsrTrampoline(void* arg) {
    GpioEsp32c3IsrTrampoline* trampoline = (GpioEsp32c3IsrTrampoline*)arg;
    GpioIsrCallback callback = NULL;
    void* userContext = NULL;
    uint32_t pin = 0;

    if (trampoline == NULL) {
        return;
    }
    if (trampoline->busy == 0) {
        return;
    }

    callback    = trampoline->callback;
    userContext = trampoline->userContext;
    pin         = trampoline->pin;

    if (callback != NULL) {
        callback(userContext, pin);
    }
}

/* ISR 服务按需安装：已安装（ESP_ERR_INVALID_STATE）视为成功，可重复调用 */
static int gpioEsp32c3EnsureIsrService(GpioEsp32c3* self) {
    esp_err_t err = ESP_OK;

    if (self == NULL) {
        return GPIO_ERR_PARAM;
    }
    if (self->isrServiceReady != 0) {
        return GPIO_OK;
    }

    /* 0 = 无特殊中断分配标志（与 IDF 默认一致） */
    err = gpio_install_isr_service(0);
    if ((err != ESP_OK) && (err != ESP_ERR_INVALID_STATE)) {
        return GPIO_ESP32C3_ERR_DRIVER;
    }

    self->isrServiceReady = 1;
    return GPIO_OK;
}

/* 释放某个引脚的中断槽位（幂等：未注册时直接返回 GPIO_OK） */
static int gpioEsp32c3ReleaseIsrSlot(GpioEsp32c3* self, uint32_t pin) {
    GpioEsp32c3IsrTrampoline* trampoline = NULL;

    if (self == NULL) {
        return GPIO_ERR_PARAM;
    }
    if (pin >= (uint32_t)GPIO_ESP32C3_ISR_SLOT_COUNT) {
        return GPIO_ERR_UNSUPPORTED;
    }

    trampoline = (GpioEsp32c3IsrTrampoline*)self->isrContexts[pin];
    if (trampoline == NULL) {
        return GPIO_OK;
    }

    /* 槽位可能已被另一个实例重新占用：只清理属于自己的绑定，
     * 不去摘别人的 handler（静态表按引脚号全局唯一）。 */
    if (trampoline->owner != self) {
        self->isrContexts[pin] = NULL;
        self->isrPinMask &= ~((uint32_t)1U << pin);
        return GPIO_OK;
    }

    /* 先摘 handler，再清槽位，避免清空后仍有中断进来拿到半旧数据 */
    (void)gpio_isr_handler_remove((gpio_num_t)pin);

    trampoline->callback    = NULL;
    trampoline->userContext = NULL;
    trampoline->owner       = NULL;
    trampoline->pin         = 0;
    trampoline->busy        = 0;

    self->isrContexts[pin] = NULL;
    self->isrPinMask &= ~((uint32_t)1U << pin);

    return GPIO_OK;
}

/* 摘除本实例注册的全部中断（析构与构造回滚共用） */
static void gpioEsp32c3ReleaseAllIsr(GpioEsp32c3* self) {
    uint32_t pin = 0;

    if (self == NULL) {
        return;
    }

    for (pin = 0; pin < (uint32_t)GPIO_ESP32C3_ISR_SLOT_COUNT; ++pin) {
        if ((self->isrPinMask & ((uint32_t)1U << pin)) != 0U) {
            (void)gpioEsp32c3ReleaseIsrSlot(self, pin);
        }
    }
}

/* ==================================================================
 * 虚表实现：九个接口（全部 static + 统一前缀，避免与 POSIX read/write 冲突）
 * ================================================================== */

static int gpioEsp32c3Init(void* instance, uint32_t pin, GpioDirection direction, GpioPull pull) {
    GpioEsp32c3* self = (GpioEsp32c3*)instance;
    gpio_config_t config;
    esp_err_t err = ESP_OK;
    int rc = GPIO_OK;

    if (self == NULL) {
        return GPIO_ERR_PARAM;
    }
    rc = gpioEsp32c3CheckPin(pin);
    if (rc != GPIO_OK) {
        return rc;
    }
    rc = gpioEsp32c3CheckDirection(direction);
    if (rc != GPIO_OK) {
        return rc;
    }
    rc = gpioEsp32c3CheckPull(pull);
    if (rc != GPIO_OK) {
        return rc;
    }

    memset(&config, 0, sizeof(config));
    config.pin_bit_mask = ((uint64_t)1U << pin);
    config.mode         = gpioEsp32c3ModeFromDirection(direction);
    config.pull_up_en   = ((pull == GpioPullUp) || (pull == GpioPullUpDown))
                              ? GPIO_PULLUP_ENABLE
                              : GPIO_PULLUP_DISABLE;
    config.pull_down_en = ((pull == GpioPullDown) || (pull == GpioPullUpDown))
                              ? GPIO_PULLDOWN_ENABLE
                              : GPIO_PULLDOWN_DISABLE;
    config.intr_type    = GPIO_INTR_DISABLE; /* 中断由 attachInterrupt 单独开启 */

    err = gpio_config(&config);
    if (err != ESP_OK) {
        return GPIO_ESP32C3_ERR_DRIVER;
    }

    /* 再按抽象枚举覆盖一次上下拉模式，保证与 GpioPull 的语义严格一致 */
    err = gpio_set_pull_mode((gpio_num_t)pin, gpioEsp32c3PullModeFromPull(pull));
    if (err != ESP_OK) {
        return GPIO_ESP32C3_ERR_DRIVER;
    }

    return GPIO_OK;
}

static int gpioEsp32c3Deinit(void* instance, uint32_t pin) {
    GpioEsp32c3* self = (GpioEsp32c3*)instance;
    esp_err_t err = ESP_OK;
    int rc = GPIO_OK;

    if (self == NULL) {
        return GPIO_ERR_PARAM;
    }
    rc = gpioEsp32c3CheckPin(pin);
    if (rc != GPIO_OK) {
        return rc;
    }

    /* 先摘中断，再复位引脚，防止复位过程中仍然回调到用户代码 */
    (void)gpioEsp32c3ReleaseIsrSlot(self, pin);

    err = gpio_reset_pin((gpio_num_t)pin);
    if (err != ESP_OK) {
        return GPIO_ESP32C3_ERR_DRIVER;
    }

    return GPIO_OK;
}

static int gpioEsp32c3SetDirection(void* instance, uint32_t pin, GpioDirection direction) {
    GpioEsp32c3* self = (GpioEsp32c3*)instance;
    esp_err_t err = ESP_OK;
    int rc = GPIO_OK;

    if (self == NULL) {
        return GPIO_ERR_PARAM;
    }
    rc = gpioEsp32c3CheckPin(pin);
    if (rc != GPIO_OK) {
        return rc;
    }
    rc = gpioEsp32c3CheckDirection(direction);
    if (rc != GPIO_OK) {
        return rc;
    }

    err = gpio_set_direction((gpio_num_t)pin, gpioEsp32c3ModeFromDirection(direction));
    if (err != ESP_OK) {
        return GPIO_ESP32C3_ERR_DRIVER;
    }

    return GPIO_OK;
}

static int gpioEsp32c3Write(void* instance, uint32_t pin, GpioLevel level) {
    GpioEsp32c3* self = (GpioEsp32c3*)instance;
    esp_err_t err = ESP_OK;
    int rc = GPIO_OK;

    if (self == NULL) {
        return GPIO_ERR_PARAM;
    }
    rc = gpioEsp32c3CheckPin(pin);
    if (rc != GPIO_OK) {
        return rc;
    }
    rc = gpioEsp32c3CheckLevel(level);
    if (rc != GPIO_OK) {
        return rc;
    }

    err = gpio_set_level((gpio_num_t)pin, (level == GpioLevelHigh) ? 1U : 0U);
    if (err != ESP_OK) {
        return GPIO_ESP32C3_ERR_DRIVER;
    }

    return GPIO_OK;
}

static int gpioEsp32c3Read(void* instance, uint32_t pin, GpioLevel* level) {
    GpioEsp32c3* self = (GpioEsp32c3*)instance;
    int rc = GPIO_OK;

    if (self == NULL || level == NULL) {
        return GPIO_ERR_PARAM;
    }
    rc = gpioEsp32c3CheckPin(pin);
    if (rc != GPIO_OK) {
        return rc;
    }

    *level = (gpio_get_level((gpio_num_t)pin) != 0) ? GpioLevelHigh : GpioLevelLow;

    return GPIO_OK;
}

static int gpioEsp32c3Toggle(void* instance, uint32_t pin) {
    GpioEsp32c3* self = (GpioEsp32c3*)instance;
    esp_err_t err = ESP_OK;
    int rc = GPIO_OK;
    int currentLevel = 0;

    if (self == NULL) {
        return GPIO_ERR_PARAM;
    }
    rc = gpioEsp32c3CheckPin(pin);
    if (rc != GPIO_OK) {
        return rc;
    }

    /* 读回当前电平后取反，兼容 GPIO_MODE_INPUT_OUTPUT 的用法 */
    currentLevel = gpio_get_level((gpio_num_t)pin);
    err = gpio_set_level((gpio_num_t)pin, (currentLevel != 0) ? 0U : 1U);
    if (err != ESP_OK) {
        return GPIO_ESP32C3_ERR_DRIVER;
    }

    return GPIO_OK;
}

static int gpioEsp32c3AttachInterrupt(void* instance, uint32_t pin, GpioEdge edge,
                                      GpioIsrCallback callback, void* context) {
    GpioEsp32c3* self = (GpioEsp32c3*)instance;
    GpioEsp32c3IsrTrampoline* trampoline = NULL;
    esp_err_t err = ESP_OK;
    int rc = GPIO_OK;

    if (self == NULL || callback == NULL) {
        return GPIO_ERR_PARAM;
    }
    rc = gpioEsp32c3CheckPin(pin);
    if (rc != GPIO_OK) {
        return rc;
    }
    rc = gpioEsp32c3CheckEdge(edge);
    if (rc != GPIO_OK) {
        return rc;
    }

    rc = gpioEsp32c3EnsureIsrService(self);
    if (rc != GPIO_OK) {
        return rc;
    }

    /* GpioEdgeNone：等价于关闭该引脚中断，不需要注册 handler */
    if (edge == GpioEdgeNone) {
        (void)gpioEsp32c3ReleaseIsrSlot(self, pin);
        err = gpio_set_intr_type((gpio_num_t)pin, GPIO_INTR_DISABLE);
        if (err != ESP_OK) {
            return GPIO_ESP32C3_ERR_DRIVER;
        }
        return GPIO_OK;
    }

    /* 重复 attach 同一引脚：先释放旧注册，保证 trampoline 与 handler 一一对应 */
    (void)gpioEsp32c3ReleaseIsrSlot(self, pin);

    err = gpio_set_intr_type((gpio_num_t)pin, gpioEsp32c3IntrTypeFromEdge(edge));
    if (err != ESP_OK) {
        return GPIO_ESP32C3_ERR_DRIVER;
    }

    trampoline              = &sGpioEsp32c3IsrTrampolines[pin];
    trampoline->callback    = callback;
    trampoline->userContext = context;
    trampoline->owner       = self;
    trampoline->pin         = pin;
    trampoline->busy        = 1;

    err = gpio_isr_handler_add((gpio_num_t)pin, gpioEsp32c3IsrTrampoline, (void*)trampoline);
    if (err != ESP_OK) {
        trampoline->callback    = NULL;
        trampoline->userContext = NULL;
        trampoline->owner       = NULL;
        trampoline->pin         = 0;
        trampoline->busy        = 0;
        return GPIO_ESP32C3_ERR_DRIVER;
    }

    self->isrContexts[pin] = (void*)trampoline;
    self->isrPinMask |= ((uint32_t)1U << pin);

    return GPIO_OK;
}

static int gpioEsp32c3DetachInterrupt(void* instance, uint32_t pin) {
    GpioEsp32c3* self = (GpioEsp32c3*)instance;
    esp_err_t err = ESP_OK;
    int rc = GPIO_OK;

    if (self == NULL) {
        return GPIO_ERR_PARAM;
    }
    rc = gpioEsp32c3CheckPin(pin);
    if (rc != GPIO_OK) {
        return rc;
    }

    /* 先摘 handler（幂等），再关引脚中断类型 */
    (void)gpioEsp32c3ReleaseIsrSlot(self, pin);

    err = gpio_set_intr_type((gpio_num_t)pin, GPIO_INTR_DISABLE);
    if (err != ESP_OK) {
        return GPIO_ESP32C3_ERR_DRIVER;
    }

    return GPIO_OK;
}

static void gpioEsp32c3DeleteInstance(void* instance) {
    GpioEsp32c3* self = (GpioEsp32c3*)instance;

    if (self == NULL) {
        return;
    }

    /* 先移除全部已注册的 ISR handler，再释放具体实例 */
    gpioEsp32c3ReleaseAllIsr(self);
    self->isrServiceReady = 0;

    if (gpioFree != NULL) {
        gpioFree(self);
    }
    self = NULL;
}

/* ==================================================================
 * 虚表 + 具体构造
 * ================================================================== */
GpioInterface GpioEsp32c3Interface(void) {
    _RETURN_GPIO_INTERFACE_WITH(gpioEsp32c3Init, gpioEsp32c3Deinit, gpioEsp32c3SetDirection,
                                gpioEsp32c3Write, gpioEsp32c3Read, gpioEsp32c3Toggle,
                                gpioEsp32c3AttachInterrupt, gpioEsp32c3DetachInterrupt,
                                gpioEsp32c3DeleteInstance);
}

Gpio* NewGpioEsp32c3(void) {
    GpioEsp32c3* self = NULL;
    Gpio* gpio = NULL;

    if (gpioMalloc == NULL) {
        return NULL;
    }

    self = (GpioEsp32c3*)gpioMalloc(sizeof(GpioEsp32c3));
    if (self == NULL) {
        return NULL;
    }

    memset(self, 0, sizeof(GpioEsp32c3));

    /* 构造即安装 ISR 服务：失败不致命（无中断需求时无影响），
     * attachInterrupt 时会再次尝试并返回明确错误码。 */
    (void)gpioEsp32c3EnsureIsrService(self);

    /* 抽象构造：只负责「分配抽象对象 + 存 instance + 存 interfaces」 */
    gpio = NewGpio((void*)self, GpioEsp32c3Interface());
    if (gpio == NULL) {
        gpioEsp32c3ReleaseAllIsr(self);
        self->isrServiceReady = 0;
        if (gpioFree != NULL) {
            gpioFree(self);
        }
        self = NULL;
        return NULL;
    }

    return gpio;
}
