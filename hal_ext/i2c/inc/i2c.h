#ifndef _I2C_H
#define _I2C_H

/* ==========================================================================
 * I2C 抽象父类（OOP in C）
 *
 * 设计要点：
 *   1. I2c 只持有 void* instance + I2cInterface interfaces，不含任何
 *      引脚号 / 端口号 / 时钟频率等平台相关字段（这些属于具象派生类）。
 *   2. 具象派生类（如 hal/i2c/src/i2c_esp32c3.c）在自己的结构体里放平台字段，
 *      把 I2cInterface 虚表注入 NewI2c，抽象层只做参数校验与转发。
 *   3. 本文件为纯抽象层：不包含任何 ESP-IDF / FreeRTOS / 平台头文件。
 *
 * 返回值约定（全部对外函数统一）：
 *    0 : 成功
 *   -1 : 参数非法（如 i2c 为空、instance 为空、data/buffer 为空、len 为 0 等）
 *   -2 : 未实现 / 不支持（该实现未提供对应的虚表槽位）
 *   -3 : 无应答 / 非法（仅 probe、read、write、writeRead 使用，由具象层返回）
 * ========================================================================== */

#include <stdint.h>
#include <stddef.h>

#if defined(__cplusplus)
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * 内存分配钩子：默认指向 malloc / free（见 i2c.c），
 * 具象模块或应用层可整体替换（如改用 pvPortMalloc / vPortFree）。
 * -------------------------------------------------------------------------- */
extern void* (*i2cMalloc)(size_t size);
extern void  (*i2cFree)(void* p);

/* --------------------------------------------------------------------------
 * 虚表：首参恒为 void* instance（具象实例），由具象层 static 函数实现
 * -------------------------------------------------------------------------- */
typedef struct I2cInterface {
    int (*init)(void* instance);
    int (*deinit)(void* instance);
    int (*write)(void* instance, uint16_t deviceAddress, const uint8_t* data, uint32_t len, uint32_t timeoutMs);
    int (*read)(void* instance, uint16_t deviceAddress, uint8_t* buffer, uint32_t len, uint32_t timeoutMs);
    int (*writeRead)(void* instance, uint16_t deviceAddress,
                     const uint8_t* writeData, uint32_t writeLen,
                     uint8_t* readBuffer, uint32_t readLen, uint32_t timeoutMs);
    int (*probe)(void* instance, uint16_t deviceAddress);
    int (*setClock)(void* instance, uint32_t clockHz);
    int (*reset)(void* instance);
    void (*deleteInstance)(void* instance);
} I2cInterface;

/* --------------------------------------------------------------------------
 * 抽象父类：只放“外部注入的具象实例” + 虚表
 * -------------------------------------------------------------------------- */
typedef struct I2c {
    void*        instance;
    I2cInterface interfaces;
} I2c;

/* --------------------------------------------------------------------------
 * 抽象构造 / 析构
 * NewI2c：只做「分配抽象对象 + 存 instance + 存 interfaces」，不创建具象实例。
 * DeleteI2c：先调 deleteInstance 销毁具象实例，再释放抽象对象，最后置 NULL。
 * -------------------------------------------------------------------------- */
I2c* NewI2c(void* instance, I2cInterface interfaces);
void DeleteI2c(I2c** pI2c);

/* --------------------------------------------------------------------------
 * 对外接口：判空 / 判非法入参后透传给具象实例
 * -------------------------------------------------------------------------- */
int I2cInit(I2c* i2c);
int I2cDeinit(I2c* i2c);
int I2cWrite(I2c* i2c, uint16_t deviceAddress, const uint8_t* data, uint32_t len, uint32_t timeoutMs);
int I2cRead(I2c* i2c, uint16_t deviceAddress, uint8_t* buffer, uint32_t len, uint32_t timeoutMs);
int I2cWriteRead(I2c* i2c, uint16_t deviceAddress,
                 const uint8_t* writeData, uint32_t writeLen,
                 uint8_t* readBuffer, uint32_t readLen, uint32_t timeoutMs);
int I2cProbe(I2c* i2c, uint16_t deviceAddress);      /* 返回 0 表示设备在线，负值表示无应答/非法 */
int I2cSetClock(I2c* i2c, uint32_t clockHz);
int I2cReset(I2c* i2c);

/* --------------------------------------------------------------------------
 * _RETURN_I2C_INTERFACE：具象层构造虚表的统一宏
 *
 * 在“返回 I2cInterface 的函数体”内直接展开为「建表 + 返回」。
 * 形参为具象层各 static 函数的函数名（顺序与虚表字段一致）。
 *
 * 用法示例（具象 .c）：
 *   I2cInterface I2cEsp32c3Interface(void)
 *   {
 *       _RETURN_I2C_INTERFACE(esp32c3Init,     esp32c3Deinit,
 *                             esp32c3Write,    esp32c3Read,
 *                             esp32c3WriteRead, esp32c3Probe,
 *                             esp32c3SetClock, esp32c3Reset,
 *                             esp32c3DeleteInstance);
 *   }
 *
 * 说明：宏以 return 语句结尾（不带分号），因此展开后函数体必然以 return 收尾，
 *       调用处结尾写 `;` 即可（多出的空语句合法）。
 * -------------------------------------------------------------------------- */
#define _RETURN_I2C_INTERFACE(initFn, deinitFn, writeFn, readFn, writeReadFn, probeFn, setClockFn, resetFn, deleteFn) \
    I2cInterface interfaces = {                                                   \
        .init           = (initFn),                                               \
        .deinit         = (deinitFn),                                             \
        .write          = (writeFn),                                              \
        .read           = (readFn),                                               \
        .writeRead      = (writeReadFn),                                          \
        .probe          = (probeFn),                                              \
        .setClock       = (setClockFn),                                           \
        .reset          = (resetFn),                                              \
        .deleteInstance = (deleteFn)                                              \
    };                                                                            \
    return interfaces

#if defined(__cplusplus)
}
#endif

#endif /* _I2C_H */
