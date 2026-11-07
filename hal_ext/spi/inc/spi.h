#ifndef _SPI_H
#define _SPI_H

/*
 * SPI 抽象层（OOP in C 父类 / 抽象类）
 * -----------------------------------------------------------------------------
 * 设计要点：
 *   1. 抽象类 Spi 只持有 { void* instance; SpiInterface interfaces; }
 *   2. SpiConfig 为纯值配置类型，可安全地跨层传递
 *   3. 本头文件【禁止】包含任何 ESP-IDF / FreeRTOS 头，
 *      只依赖 C 标准头 <stdint.h> / <stddef.h>，
 *      使用方（main、services 等）仅面向本抽象编程，不会被拖入平台头文件链
 *   4. 具体实现（如 hal/spi/src/spi_esp32c3.c）在 NewXxx 内部调用 NewSpi 完成装配
 *
 * 返回值约定：
 *   transfer / write / read : 返回实际传输字节数（>= 0），负值表示错误
 *   其余函数                :  0 成功 / -1 参数非法 / -2 未实现（接口函数指针为空）
 */

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========== ① 内存分配器（默认 malloc / free，可被外部替换）========== */
extern void* (*spiMalloc)(size_t size);
extern void  (*spiFree)(void* p);

/* ========== ② SPI 模式：CPOL / CPHA 组合 ========== */
/* 枚举常量使用 CamelCase，避免与 IDF 全大写枚举 / 宏（如 SPI_MODE0）冲突 */
typedef enum SpiMode {
    SpiMode0 = 0,   /* CPOL = 0, CPHA = 0 */
    SpiMode1,       /* CPOL = 0, CPHA = 1 */
    SpiMode2,       /* CPOL = 1, CPHA = 0 */
    SpiMode3        /* CPOL = 1, CPHA = 1 */
} SpiMode;

/* ========== ③ SPI 配置（纯值类型）========== */
typedef struct SpiConfig {
    uint32_t clockHz;      /* 时钟频率 */
    SpiMode  mode;         /* CPOL / CPHA */
    uint8_t  bitsPerWord;  /* 通常 8 */
} SpiConfig;

/* ========== ④ 接口表（虚表 V-Table）==========
 * 虚函数首参统一为 void* instance（具体实例），实现侧内部写：
 *     Spi* self = (Spi*)instance;    // 注意：禁止使用 C++ 保留字作标识符
 * 【必需】析构项字段名固定为 deleteInstance，禁止命名为 C++ 保留字。
 */
typedef struct SpiInterface {
    int (*init)(void* instance);
    int (*deinit)(void* instance);
    int (*transfer)(void* instance, const uint8_t* txData, uint8_t* rxData, uint32_t len, uint32_t timeoutMs);
    int (*write)(void* instance, const uint8_t* txData, uint32_t len, uint32_t timeoutMs);
    int (*read)(void* instance, uint8_t* rxData, uint32_t len, uint32_t timeoutMs);
    int (*select)(void* instance, int active);     /* 片选：active 非 0 = 拉低选中 */
    int (*setConfig)(void* instance, const SpiConfig* config);
    void (*deleteInstance)(void* instance);
} SpiInterface;

/* ========== ⑤ 抽象类结构体 ========== */
typedef struct Spi {
    void*        instance;    /* 指向具体实现的实例（多态核心）*/
    SpiInterface interfaces;  /* 接口函数表 */
} Spi;

/* ========== ⑥ 抽象类构造 / 析构 ==========
 * NewSpi 只做「分配抽象对象 + 存 instance + 存接口表」，
 * 禁止在此创建具体实例；具体实例由具体类的 NewXxx 创建后传入。
 */
Spi* NewSpi(void* instance, SpiInterface interfaces);
void DeleteSpi(Spi** pSpi);

/* ========== ⑦ 默认配置：mode0、8bit ========== */
SpiConfig SpiDefaultConfig(uint32_t clockHz);

/* ========== ⑧ 面向抽象类的操作（先判空再透传到具体实现）========== */
int SpiInit(Spi* spi);
int SpiDeinit(Spi* spi);
/* transfer 允许 txData 或 rxData 之一为 NULL（单向收发），但不可同时为 NULL */
int SpiTransfer(Spi* spi, const uint8_t* txData, uint8_t* rxData, uint32_t len, uint32_t timeoutMs);
int SpiWrite(Spi* spi, const uint8_t* txData, uint32_t len, uint32_t timeoutMs);
int SpiRead(Spi* spi, uint8_t* rxData, uint32_t len, uint32_t timeoutMs);
int SpiSelect(Spi* spi, int active);
int SpiSetConfig(Spi* spi, const SpiConfig* config);

/* ========== ⑨ 接口表集中返回宏（供具象实现 .c 直接使用）==========
 * 用法（具象 .c 内，函数返回类型必须为 SpiInterface）：
 *     static SpiInterface SpiEsp32c3Interface(void) {
 *         _RETURN_SPI_INTERFACE;                       // 约定函数名与虚表一致时
 *     }
 *     static SpiInterface SpiEsp32c3Interface(void) {
 *         _RETURN_SPI_INTERFACE_BY(SpiEsp32c3Init, SpiEsp32c3Deinit,
 *                                  SpiEsp32c3Transfer, SpiEsp32c3Write,
 *                                  SpiEsp32c3Read, SpiEsp32c3Select,
 *                                  SpiEsp32c3SetConfig, SpiEsp32c3Destroy);
 *     }
 */
#define _RETURN_SPI_INTERFACE_BY(fnInit, fnDeinit, fnTransfer, fnWrite, fnRead, fnSelect, fnSetConfig, fnDestroy) \
    do {                                                                                                          \
        SpiInterface interfaces = {                                                                               \
            .init           = (fnInit),                                                                           \
            .deinit         = (fnDeinit),                                                                         \
            .transfer       = (fnTransfer),                                                                       \
            .write          = (fnWrite),                                                                          \
            .read           = (fnRead),                                                                           \
            .select         = (fnSelect),                                                                         \
            .setConfig      = (fnSetConfig),                                                                      \
            .deleteInstance = (fnDestroy)                                                                         \
        };                                                                                                        \
        return interfaces;                                                                                        \
    } while (0)

/* 约定具象侧函数名恰为 init/deinit/transfer/write/read/select/setConfig/deleteInstance 时使用 */
#define _RETURN_SPI_INTERFACE \
    _RETURN_SPI_INTERFACE_BY(init, deinit, transfer, write, read, select, setConfig, deleteInstance)

#ifdef __cplusplus
}
#endif

#endif /* _SPI_H */
