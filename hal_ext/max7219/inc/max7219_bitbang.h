#ifndef _MAX7219_BITBANG_H
#define _MAX7219_BITBANG_H

/* ==================================================================
 * max7219_bitbang.h —— MAX7219 抽象父类（max7219.h）的具象派生类：
 *                     3 线软件位带（Bit-Bang）实现，平台无关。
 *
 * 结构（OOP in C）：
 *   父类  Max7219        = { void* instance; Max7219Interface interfaces; }
 *   本类  Max7219BitBang = 三个引脚号 + 级联片数 + 延时钩子 + 整帧缓冲
 *
 *   NewMax7219BitBang() 内部创建 Max7219BitBang 实例，再调用
 *   NewMax7219(ins, Max7219BitBangInterface()) 完成装配；
 *   使用方只面向 Max7219* 抽象编程，全程不接触具体类型。
 *
 * 平台无关性（本模块的立身之本）：
 *   1. 只依赖抽象父类 max7219.h 与 GPIO 抽象 gpio.h，
 *      禁止包含 driver、freertos 下的平台头与 esp_*.h。
 *   2. 引脚操作一律经 GpioWrite / GpioInit 走虚表，不直接碰寄存器。
 *   3. 微秒延时由调用方以函数指针注入（delayUs），
 *      传 NULL 表示不额外延时，用于软件开销本身已足够慢的场合。
 *
 * 返回值约定（与抽象父类一致）：
 *    0   成功
 *   -1   参数非法 / 级联索引越界 / 帧长与链长不匹配
 *   -2   未实现（本模块九个槽位全部实现，故不会主动返回 -2；
 *        若出现 -2，必为底层 Gpio 上抛的 GPIO_ERR_UNSUPPORTED）
 *   底层 GpioInit / GpioWrite 失败时，其负值原样上抛，不二次编码。
 *
 * 级联顺序（继承自抽象层语义约定）：
 *   chipIndex = 0 表示【直接接 DIN 的第一片】（离 MCU 最近）。
 *   writeFrame 的 frame 缓冲区按「先发最远端」排列：
 *       frame[0 .. 1]                                   -> chipIndex = chainLength - 1
 *       ...
 *       frame[2 * chainLength - 2 .. 2 * chainLength - 1] -> chipIndex = 0
 *   本模块不重排、不反转，按字节顺序 MSB first 原样移出。
 *
 * 所有权约定：
 *   Gpio* 由调用方创建并拥有，本模块只借用，析构时【不】调用 DeleteGpio。
 *   frameBuffer 由本模块在构造时分配，析构时经 max7219Free 释放。
 * ================================================================== */

#include <stdint.h>

#include "max7219.h"
#include "gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------
 * 返回值约定宏（与抽象父类数值保持一致）
 * ------------------------------------------------------------------ */
#define MAX7219_BITBANG_OK              ( 0)
#define MAX7219_BITBANG_ERR_PARAM       (-1)
#define MAX7219_BITBANG_ERR_UNSUPPORTED (-2)

/* ------------------------------------------------------------------
 * 构造配置：外部依赖注入
 *
 *   gpio        : 调用方拥有的 GPIO 抽象对象；为空则构造失败
 *   dinPin      : 数据脚，移出时先置位再打时钟上升沿
 *   clkPin      : 时钟脚，空闲低电平，上升沿移入一位
 *   loadPin     : 锁存脚，低电平开启移位窗口，上升沿锁存已移入的 16 位
 *   chainLength : 级联片数，必须大于等于 1（本模块上限 1024）
 *   delayUs     : 每个电平边沿之后的额外延时（微秒）；传 NULL 不延时
 * ------------------------------------------------------------------ */
typedef struct Max7219BitBangConfig {
    Gpio*    gpio;
    uint32_t dinPin;
    uint32_t clkPin;
    uint32_t loadPin;
    uint32_t chainLength;
    void   (*delayUs)(uint32_t microseconds);
} Max7219BitBangConfig;

/* ------------------------------------------------------------------
 * 具象数据成员：只允许中性类型（uint32_t / uint8_t / 函数指针 / 抽象指针），
 * 不出现任何平台句柄。
 *
 *   frameBuffer : 2 * chainLength 字节的整帧缓冲，
 *                 每个寄存器写占 2 字节【地址 + 数据】，
 *                 按「先发最远端」排列，归本实例所有。
 * ------------------------------------------------------------------ */
typedef struct Max7219BitBang {
    Gpio*    gpio;        /* 外部注入，本模块不销毁 */
    uint32_t dinPin;
    uint32_t clkPin;
    uint32_t loadPin;
    uint32_t chainLength;
    void   (*delayUs)(uint32_t microseconds);
    uint8_t* frameBuffer;
} Max7219BitBang;

/* ------------------------------------------------------------------
 * 具体构造：分配具象实例 + 分配整帧缓冲 + 拷贝配置
 *           + 内部调用 NewMax7219() 完成抽象装配。
 *
 * 本函数【不】操作任何引脚（不上电、不下发寄存器），
 * 硬件上电默认配置由后续的 Max7219Init() 完成。
 *
 * 返回：装配好的 Max7219* 抽象对象；
 *       config 为空、gpio 为空、chainLength 为 0、引脚重复、
 *       内存不足或抽象构造失败时返回 NULL（失败路径全部回滚，不泄漏）。
 * ------------------------------------------------------------------ */
Max7219* NewMax7219BitBang(const Max7219BitBangConfig* config);

/* ------------------------------------------------------------------
 * 虚表集中返回：字段顺序严格遵循 Max7219Interface 定义
 * 供 NewMax7219 装配与「外部实例注入」场景复用。
 * ------------------------------------------------------------------ */
Max7219Interface Max7219BitBangInterface(void);

#ifdef __cplusplus
}
#endif

#endif /* _MAX7219_BITBANG_H */
