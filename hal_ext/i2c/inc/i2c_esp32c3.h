#ifndef _I2C_ESP32C3_H
#define _I2C_ESP32C3_H

/* ==========================================================================
 * I2C 具象派生类：ESP32-C3（OOP in C）
 *
 * 设计要点：
 *   1. 本类派生自抽象父类 I2c（见 hal/i2c/inc/i2c.h），只提供平台相关的
 *      端口 / 引脚 / 时钟字段，并把 I2cInterface 虚表注入抽象对象。
 *   2. 【硬约束】本头文件禁止包含任何 ESP-IDF / FreeRTOS 头，
 *      结构体字段只使用 uint32_t / uint8_t / void*，
 *      保证使用方（main / services）面向本头编程时不被拖入平台头文件链。
 *   3. 平台类型（i2c_port_num_t、i2c_cmd_handle_t ...）只出现在 .c 内部。
 *   4. 具象构造 NewI2cEsp32c3() 内部依次完成：
 *        i2cMalloc → memset → 填端口/引脚/时钟
 *        → i2c_param_config + i2c_driver_install（失败则释放并返回 NULL）
 *        → NewI2c(ins, I2cEsp32c3Interface())（抽象构造装配，失败则释放并返回 NULL）
 *      因此 NewI2cEsp32c3() 返回后总线即处于可用状态，I2cInit() 为幂等补装。
 *
 * 返回值约定（与 i2c.h 一致）：
 *    0 : 成功
 *   -1 : 参数非法 / 端口非法 / 总线未安装
 *   -2 : 未实现（本实现不返回该值，保留语义）
 *   -3 : 无应答（NACK）或总线超时失败（write / read / writeRead / probe）
 * ========================================================================== */

#include <stdint.h>

#include "i2c.h"

#if defined(__cplusplus)
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * ESP32-C3 常用默认值（示例板：SDA = GPIO8，SCL = GPIO9，100kHz）
 * 仅为方便调用方的编译期常量，不做任何强约束。
 * -------------------------------------------------------------------------- */
#define I2C_ESP32C3_DEFAULT_SDA_PIN      (8U)
#define I2C_ESP32C3_DEFAULT_SCL_PIN      (9U)
#define I2C_ESP32C3_DEFAULT_CLOCK_HZ     (100000U)

/* --------------------------------------------------------------------------
 * 具象实例：只放平台相关字段（抽象父类只持有 void* instance + 虚表）
 * -------------------------------------------------------------------------- */
typedef struct I2cEsp32c3 {
    uint32_t port;      /* 端口号：ESP32-C3 仅有 I2C_NUM_0（即 0） */
    uint32_t sdaPin;    /* SDA 的 GPIO 编号 */
    uint32_t sclPin;    /* SCL 的 GPIO 编号 */
    uint32_t clockHz;   /* 当前总线时钟（Hz），100000 / 400000 常用 */
    uint8_t  installed; /* 1 = legacy 主机驱动已安装，可安全卸载 */
} I2cEsp32c3;

/* --------------------------------------------------------------------------
 * 具象构造 / 接口表
 *   NewI2cEsp32c3：自定义参数（端口 + 引脚 + 时钟），内部调用抽象构造 NewI2c
 *   I2cEsp32c3Interface：返回注入抽象对象的虚表（函数名与字段顺序见 i2c.h）
 * -------------------------------------------------------------------------- */
I2c* NewI2cEsp32c3(uint32_t port, uint32_t sdaPin, uint32_t sclPin, uint32_t clockHz);
I2cInterface I2cEsp32c3Interface(void);

#if defined(__cplusplus)
}
#endif

#endif /* _I2C_ESP32C3_H */
