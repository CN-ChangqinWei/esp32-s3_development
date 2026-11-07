#ifndef _GPIO_H
#define _GPIO_H

/* ==================================================================
 * gpio.h —— GPIO 抽象父类（OOP in C）
 *
 * 结构：
 *   抽象对象 Gpio = { void* instance; GpioInterface interfaces; }
 *   具体实现（gpio_esp32c3 等）负责 instance 的真实类型与全部接口函数，
 *   抽象层只负责「装配 + 判空 + 透传」，不含任何业务字段。
 *
 * 依赖：
 *   只允许 <stdint.h> / <stddef.h>，
 *   禁止包含 freertos/* 、driver/* 、esp_*.h 等平台头。
 *
 * 返回值约定：
 *    0   成功
 *   -1   参数非法
 *   -2   未实现 / 不支持
 *   其余负值为具体错误码
 * ================================================================== */

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------
 * 内存钩子：默认指向 malloc / free（定义在 gpio.c），
 * 外部可在创建对象之前替换为平台专用分配器：
 *     gpioMalloc = pvPortMalloc;  gpioFree = vPortFree;
 * ------------------------------------------------------------------ */
extern void* (*gpioMalloc)(size_t size);
extern void  (*gpioFree)(void* p);

/* ------------------------------------------------------------------
 * 返回值约定宏
 * ------------------------------------------------------------------ */
#define GPIO_OK                 0
#define GPIO_ERR_PARAM        (-1)
#define GPIO_ERR_UNSUPPORTED  (-2)

/* ------------------------------------------------------------------
 * 枚举：方向 / 上下拉 / 电平 / 中断边沿
 * ------------------------------------------------------------------ */
typedef enum GpioDirection {
    GpioDirInput       = 0,
    GpioDirOutput      = 1,
    GpioDirInputOutput = 2
} GpioDirection;

typedef enum GpioPull {
    GpioPullNone   = 0,
    GpioPullUp     = 1,
    GpioPullDown   = 2,
    GpioPullUpDown = 3
} GpioPull;

typedef enum GpioLevel {
    GpioLevelLow  = 0,
    GpioLevelHigh = 1
} GpioLevel;

typedef enum GpioEdge {
    GpioEdgeNone    = 0,
    GpioEdgeRising  = 1,
    GpioEdgeFalling = 2,
    GpioEdgeAny     = 3
} GpioEdge;

/* 中断回调：context 为 attach 时透传的用户上下文，pin 为触发引脚号 */
typedef void (*GpioIsrCallback)(void* context, uint32_t pin);

/* ------------------------------------------------------------------
 * 虚表：由具体实现填充；抽象层只调用，不实现
 * ------------------------------------------------------------------ */
typedef struct GpioInterface {
    int  (*init)(void* instance, uint32_t pin, GpioDirection direction, GpioPull pull);
    int  (*deinit)(void* instance, uint32_t pin);
    int  (*setDirection)(void* instance, uint32_t pin, GpioDirection direction);
    int  (*write)(void* instance, uint32_t pin, GpioLevel level);
    int  (*read)(void* instance, uint32_t pin, GpioLevel* level);
    int  (*toggle)(void* instance, uint32_t pin);
    int  (*attachInterrupt)(void* instance, uint32_t pin, GpioEdge edge, GpioIsrCallback callback, void* context);
    int  (*detachInterrupt)(void* instance, uint32_t pin);
    void (*deleteInstance)(void* instance);
} GpioInterface;

/* ------------------------------------------------------------------
 * 抽象对象：只允许 instance + interfaces 两个字段
 * ------------------------------------------------------------------ */
typedef struct Gpio {
    void*         instance;
    GpioInterface interfaces;
} Gpio;

/* ==================================================================
 * _RETURN_GPIO_INTERFACE
 * 供具体实现集中返回接口表，要求实现侧存在以下同名 static 函数：
 *
 *   static int  init(void* instance, uint32_t pin, GpioDirection direction, GpioPull pull);
 *   static int  deinit(void* instance, uint32_t pin);
 *   static int  setDirection(void* instance, uint32_t pin, GpioDirection direction);
 *   static int  write(void* instance, uint32_t pin, GpioLevel level);
 *   static int  read(void* instance, uint32_t pin, GpioLevel* level);
 *   static int  toggle(void* instance, uint32_t pin);
 *   static int  attachInterrupt(void* instance, uint32_t pin, GpioEdge edge,
 *                               GpioIsrCallback callback, void* context);
 *   static int  detachInterrupt(void* instance, uint32_t pin);
 *   static void deleteInstance(void* instance);
 *
 * 用法（具体实现文件里）：
 *   GpioInterface GpioEsp32c3Interface(void) {
 *       _RETURN_GPIO_INTERFACE;
 *   }
 *
 * 备注：若平台头（如 POSIX 的 <unistd.h>）已声明 read / write，
 *       可改用带参版本 _RETURN_GPIO_INTERFACE_WITH(...) 传入自己的函数名。
 * ================================================================== */
#define _RETURN_GPIO_INTERFACE_WITH(initFunc, deinitFunc, setDirectionFunc, writeFunc, readFunc, \
                                    toggleFunc, attachInterruptFunc, detachInterruptFunc, deleteFunc) \
    GpioInterface interfaces = {                   \
        .init            = (initFunc),             \
        .deinit          = (deinitFunc),           \
        .setDirection    = (setDirectionFunc),     \
        .write           = (writeFunc),            \
        .read            = (readFunc),             \
        .toggle          = (toggleFunc),           \
        .attachInterrupt = (attachInterruptFunc),  \
        .detachInterrupt = (detachInterruptFunc),  \
        .deleteInstance  = (deleteFunc),           \
    };                                             \
    return interfaces

#define _RETURN_GPIO_INTERFACE                                                     \
    _RETURN_GPIO_INTERFACE_WITH(init, deinit, setDirection, write, read, toggle,    \
                                attachInterrupt, detachInterrupt, deleteInstance)

/* ------------------------------------------------------------------
 * 抽象构造 / 抽象析构
 * ------------------------------------------------------------------ */
Gpio* NewGpio(void* instance, GpioInterface interfaces);
void  DeleteGpio(Gpio** pGpio);

/* ------------------------------------------------------------------
 * 包装函数：判空 + 透传虚表
 * ------------------------------------------------------------------ */
int GpioInit(Gpio* gpio, uint32_t pin, GpioDirection direction, GpioPull pull);
int GpioDeinit(Gpio* gpio, uint32_t pin);
int GpioSetDirection(Gpio* gpio, uint32_t pin, GpioDirection direction);
int GpioWrite(Gpio* gpio, uint32_t pin, GpioLevel level);
int GpioRead(Gpio* gpio, uint32_t pin, GpioLevel* level);
int GpioToggle(Gpio* gpio, uint32_t pin);
int GpioAttachInterrupt(Gpio* gpio, uint32_t pin, GpioEdge edge, GpioIsrCallback callback, void* context);
int GpioDetachInterrupt(Gpio* gpio, uint32_t pin);

#ifdef __cplusplus
}
#endif

#endif /* _GPIO_H */
