#ifndef _MAX7219_H
#define _MAX7219_H

/*
 * MAX7219 抽象层（OOP in C 父类 / 抽象类）
 * -----------------------------------------------------------------------------
 * 设计要点：
 *   1. 抽象类 Max7219 只持有 { void* instance; Max7219Interface interfaces; }
 *      —— 引脚号、片数、SPI/GPIO 句柄等一律不进抽象结构体，全部归具象实例。
 *   2. 本头文件【零平台依赖】：只依赖 C 标准头 <stdint.h> / <stddef.h>，
 *      不引用任何 FreeRTOS / ESP-IDF / GPIO 头，调用方只面向本抽象编程。
 *      实现文件（max7219.c）额外只用到 <stdlib.h>（malloc / free 的默认值）。
 *   3. 具体实现（如产品侧 max7219_spi 具象）在自己的 NewXxx 内完成实例装配，
 *      再调用 NewMax7219 组装抽象对象；接口表用本头文件的
 *      _RETURN_MAX7219_INTERFACE / _RETURN_MAX7219_INTERFACE_WITH 统一返回。
 *   4. 抽象层只做「判空 + 判范围 + 透传」，不做任何时序 / 寄存器编解码 /
 *      片选控制，这些都属于具象层职责。
 *
 * 返回值约定（本模块所有 int 返回值）：
 *    0  成功
 *   -1  参数非法：driver 为空、instance 为空、intensity 大于 15、
 *      digits 不在 1 到 8、frame 为空、len 为 0 或为奇数
 *      （chipIndex 是否越界由具象层判定，抽象层只透传）
 *   -2  虚表槽位为空（该能力未实现）
 *
 * 语义约定（抽象层与具象层共同遵守）：
 *   ① 级联顺序：chipIndex = 0 表示【直接接 DIN 的第一片】（离 MCU 最近的那片）。
 *      writeFrame 的 frame 缓冲区按「先发最远端」排列：
 *        frame[0]、frame[1]  -> chipIndex = chainLength - 1
 *        ...
 *        frame[2 * chainLength - 2]、frame[2 * chainLength - 1] -> chipIndex = 0
 *      这是 MAX7219 移位寄存器级联的固有顺序（先入者被推到链尾），
 *      抽象层不重排、不反转，原样交给具象层。
 *   ② writeFrame 的 len 必须是偶数，且等于 2 * chainLength（每个寄存器
 *      写操作占 2 字节：地址 + 数据）。该长度与链长的匹配校验由具象层完成。
 *   ③ init 的语义：由具象层决定「上电默认配置」（建议 decode = 0x00、
 *      scanLimit = 8、intensity 取中等值、退出 shutdown 模式），
 *      抽象层只透传，不注入任何默认值。
 *   ④ shutdown(enable)：enable 非 0 表示进入 shutdown 模式（关闭显示），
 *      enable 为 0 表示退出 shutdown 恢复正常显示。
 *   ⑤ setDecodeMode：0x00 表示不译码（原始段码），0xFF 表示 BCD 全译码。
 *      该参数为 8 位位图，任何取值都合法，抽象层不做范围校验。
 */

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========== ① 内存分配器（默认 malloc / free，可被外部整体替换）==========
 * 更换示例（在创建任何对象之前执行）：
 *     max7219Malloc = pvPortMalloc;   max7219Free = vPortFree;
 */
extern void* (*max7219Malloc)(size_t size);
extern void  (*max7219Free)(void* p);

/* ========== ② 接口表（虚表 V-Table）==========
 * 虚函数首参统一为 void* instance（具体实例），实现侧内部写：
 *     Max7219* self = (Max7219*)instance;   // 禁止使用 C++ 保留字作标识符
 * 【必需】析构项字段名固定为 deleteInstance，不可命名为 C++ 保留字。
 */
typedef struct Max7219Interface {
    int (*init)(void* instance);
    int (*shutdown)(void* instance, int enable);            /* enable 非 0 -> 关闭显示（shutdown 模式）*/
    int (*setIntensity)(void* instance, uint8_t intensity); /* 亮度 0 到 15 */
    int (*setScanLimit)(void* instance, uint8_t digits);    /* 有效扫描位数 1 到 8 */
    int (*setDecodeMode)(void* instance, uint8_t mode);     /* 0x00 = 不译码；0xFF = BCD 全译码 */
    int (*clear)(void* instance);                           /* 清空全部显示寄存器 */
    int (*writeRegister)(void* instance, uint32_t chipIndex, uint8_t reg, uint8_t value);
    int (*writeFrame)(void* instance, const uint8_t* frame, uint32_t len);
    void (*deleteInstance)(void* instance);
} Max7219Interface;

/* ========== ③ 抽象类结构体（只允许 instance + interfaces 两个字段）========== */
typedef struct Max7219 {
    void*            instance;   /* 指向具体实现的实例（多态核心）*/
    Max7219Interface interfaces; /* 接口函数表 */
} Max7219;

/* ========== ④ 抽象类构造 / 析构 ==========
 * NewMax7219 只做「分配抽象对象 + 存 instance + 存接口表」，
 * 不创建具体实例、不解析任何业务参数；instance 为空或
 * interfaces.deleteInstance 为空时返回 NULL。
 * DeleteMax7219 先销毁具体实例，再释放抽象对象，最后把外部指针置空。
 */
Max7219* NewMax7219(void* instance, Max7219Interface interfaces);
void     DeleteMax7219(Max7219** pMax7219);

/* ========== ⑤ 面向抽象类的操作（先判空 / 判范围，再透传到具体实现）========== */
int Max7219Init(Max7219* driver);
int Max7219Shutdown(Max7219* driver, int enable);
int Max7219SetIntensity(Max7219* driver, uint8_t intensity);
int Max7219SetScanLimit(Max7219* driver, uint8_t digits);
int Max7219SetDecodeMode(Max7219* driver, uint8_t mode);
int Max7219Clear(Max7219* driver);
int Max7219WriteRegister(Max7219* driver, uint32_t chipIndex, uint8_t reg, uint8_t value);
/* frame 按「先发最远端」排列，len 必须为偶数且等于 2 * chainLength（见文件头语义约定）*/
int Max7219WriteFrame(Max7219* driver, const uint8_t* frame, uint32_t len);

/* ========== ⑥ 接口表集中返回宏（供具象实现 .c 直接使用）==========
 * 用法（具象 .c 内，函数返回类型必须为 Max7219Interface）：
 *     static Max7219Interface Max7219SpiInterface(void) {
 *         _RETURN_MAX7219_INTERFACE;                    // 函数名符合约定时（末尾带分号）
 *     }
 *     static Max7219Interface Max7219SpiInterface(void) {
 *         _RETURN_MAX7219_INTERFACE_WITH(max7219SpiInit, max7219SpiShutdown,
 *                                        max7219SpiSetIntensity, max7219SpiSetScanLimit,
 *                                        max7219SpiSetDecodeMode, max7219SpiClear,
 *                                        max7219SpiWriteRegister, max7219SpiWriteFrame,
 *                                        max7219SpiDestroy);   // 末尾带分号
 *     }
 * 注意：宏展开以 return interfaces 结尾，不带分号，调用处需自行补分号。
 */
#define _RETURN_MAX7219_INTERFACE_WITH(initFn, shutdownFn, setIntensityFn, setScanLimitFn, \
                                       setDecodeModeFn, clearFn, writeRegisterFn,           \
                                       writeFrameFn, deleteFn)                              \
    Max7219Interface interfaces = {                                                         \
        .init           = (initFn),                                                         \
        .shutdown       = (shutdownFn),                                                     \
        .setIntensity   = (setIntensityFn),                                                 \
        .setScanLimit   = (setScanLimitFn),                                                 \
        .setDecodeMode  = (setDecodeModeFn),                                                \
        .clear          = (clearFn),                                                        \
        .writeRegister  = (writeRegisterFn),                                                \
        .writeFrame     = (writeFrameFn),                                                   \
        .deleteInstance = (deleteFn)                                                        \
    };                                                                                      \
    return interfaces

/* 约定具象侧函数名恰为 init / shutdown / setIntensity / setScanLimit /
 * setDecodeMode / clear / writeRegister / writeFrame / deleteInstance 时使用 */
#define _RETURN_MAX7219_INTERFACE                                                           \
    _RETURN_MAX7219_INTERFACE_WITH(init, shutdown, setIntensity, setScanLimit,              \
                                   setDecodeMode, clear, writeRegister, writeFrame,          \
                                   deleteInstance)

#ifdef __cplusplus
}
#endif

#endif /* _MAX7219_H */
