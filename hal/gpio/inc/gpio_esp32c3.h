#ifndef _GPIO_ESP32C3_H
#define _GPIO_ESP32C3_H

/* ==================================================================
 * gpio_esp32c3.h —— GPIO 抽象父类（gpio.h）的 ESP32-C3 具象派生类
 *
 * 依赖约定：
 *   本头文件只包含抽象父类 gpio.h（以及 <stdint.h>），
 *   禁止包含 driver/* 、freertos/* 、esp_*.h 等平台头，
 *   因此使用方（main / 业务层）不会被拖入平台头文件链，
 *   hal 组件可以继续用 PRIV_REQUIRES 隐藏驱动依赖。
 *   平台类型（esp_err_t / gpio_num_t / gpio_config_t …）只出现在
 *   gpio_esp32c3.c 内部。
 *
 * 平台映射（ESP32-C3，有效 GPIO 为 0~21）：
 *   GpioDirInput        -> GPIO_MODE_INPUT
 *   GpioDirOutput       -> GPIO_MODE_OUTPUT
 *   GpioDirInputOutput  -> GPIO_MODE_INPUT_OUTPUT
 *
 *   GpioPullNone        -> GPIO_FLOATING
 *   GpioPullUp          -> GPIO_PULLUP_ONLY
 *   GpioPullDown        -> GPIO_PULLDOWN_ONLY
 *   GpioPullUpDown      -> GPIO_PULLUP_PULLDOWN
 *
 *   GpioEdgeNone        -> GPIO_INTR_DISABLE   （等价于关闭该引脚中断）
 *   GpioEdgeRising      -> GPIO_INTR_POSEDGE
 *   GpioEdgeFalling     -> GPIO_INTR_NEGEDGE
 *   GpioEdgeAny         -> GPIO_INTR_ANYEDGE
 *
 * 返回值：
 *    GPIO_OK(0)                   成功
 *    GPIO_ERR_PARAM(-1)           参数非法（空指针 / 引脚非法 / 枚举越界）
 *    GPIO_ERR_UNSUPPORTED(-2)     本芯片未实现的槽位（无对应中断槽位）
 *    GPIO_ESP32C3_ERR_DRIVER(-3)  底层驱动（esp_err_t）返回错误
 *
 * 用法：
 *    Gpio* gpio = NewGpioEsp32c3();
 *    GpioInit(gpio, 4, GpioDirOutput, GpioPullNone);
 *    GpioWrite(gpio, 4, GpioLevelHigh);
 *    DeleteGpio(&gpio);          // 内部经虚表 deleteInstance 释放具体实例
 * ================================================================== */

#include <stdint.h>

#include "gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ESP32-C3 有效 GPIO 为 0~21 */
#define GPIO_ESP32C3_VALID_PIN_COUNT   (22)

/* 中断转发槽位固定 24 个（覆盖 0~21 并留余量）：
 * 静态分配、按引脚号索引，ISR 内部只读这块内存，不做任何堆操作。 */
#define GPIO_ESP32C3_ISR_SLOT_COUNT    (24)

/* 具象实现私有返回码：底层驱动调用失败（抽象层约定之外的负值） */
#define GPIO_ESP32C3_ERR_DRIVER        (-3)

/* ------------------------------------------------------------------
 * 具象数据成员：只用中性类型（uint8_t / uint32_t / void*），
 * 平台句柄一律不出现。
 *
 *   isrContexts[pin] : 指向 .c 内静态中断转发器槽位的地址，
 *                      NULL 表示该引脚当前未挂中断。
 *   isrPinMask       : 本实例已注册中断的引脚位图，供 deleteInstance
 *                      一次性摘除全部 handler。
 *   isrServiceReady  : gpio_install_isr_service 是否已就绪（0/1）。
 * ------------------------------------------------------------------ */
typedef struct GpioEsp32c3 {
    void*    isrContexts[GPIO_ESP32C3_ISR_SLOT_COUNT];
    uint32_t isrPinMask;
    uint8_t  isrServiceReady;
} GpioEsp32c3;

/* ------------------------------------------------------------------
 * 具体构造：内部完成「分配具体实例 + 初始化字段 +
 *            调用抽象构造 NewGpio(ins, GpioEsp32c3Interface()) 装配」。
 * 失败（内存不足 / 抽象构造失败）返回 NULL，且不泄漏具体实例。
 * ------------------------------------------------------------------ */
Gpio* NewGpioEsp32c3(void);

/* 虚表：供 NewGpio 装配与「外部实例注入」场景复用 */
GpioInterface GpioEsp32c3Interface(void);

#ifdef __cplusplus
}
#endif

#endif /* _GPIO_ESP32C3_H */
