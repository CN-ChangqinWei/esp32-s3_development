/**
 * @file max7219_spi.c
 * @brief MAX7219 具象派生类实现：经抽象 SPI 总线整帧下发，平台无关。
 *
 * 职责边界：
 *   ① NewMax7219Spi()：校验入参 -> 分配 Max7219Spi -> 分配整帧缓冲
 *      -> 拷贝配置（超时为 0 时归一为默认 100）-> 调用 NewMax7219() 装配抽象父类
 *      （任一步失败全回滚）
 *   ② 接口函数全部 static，首参恒为 void* instance，首行取具体实例 self
 *   ③ 总线操作一律经 SPI 抽象（SpiSelect / SpiTransfer），不碰任何寄存器与引脚
 *   ④ 内存一律走 max7219Malloc / max7219Free（可在外部整体替换）
 *
 * 硬约束：本文件不含任何平台代码，只 include 标准头与三份抽象头，
 *         禁止引入 driver 目录、freertos 目录下的平台头与 esp_ 前缀头，
 *         也不引入 gpio 抽象 —— 片选由注入的 Spi 具象负责。
 *
 * 返回值约定（与 max7219.h 一致）：
 *    0  成功
 *   -1  参数非法 / chipIndex 越界 / len 与链长不匹配 / 短传输（非负且不等于整帧字节数）
 *   -2  未实现（九槽位全实现，本模块不主动返回；出现即底层 Spi 虚表槽位为空上抛）
 *   底层 SpiSelect / SpiTransfer 失败时其负值原样上抛。
 */

#include "max7219_spi.h"

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "max7219.h"
#include "spi.h"

/* ==================================================================
 * 内部常量
 * ================================================================== */

/* MAX7219 寄存器地址 */
#define MAX7219_SPI_REG_DIGIT0      (0x01u)   /* DIGIT0，DIGIT1..DIGIT7 依次 +1 */
#define MAX7219_SPI_REG_DECODE_MODE (0x09u)
#define MAX7219_SPI_REG_INTENSITY   (0x0Au)
#define MAX7219_SPI_REG_SCAN_LIMIT  (0x0Bu)
#define MAX7219_SPI_REG_SHUTDOWN    (0x0Cu)
#define MAX7219_SPI_REG_DISPLAY_TEST (0x0Fu)  /* Display Test（地址 0x0F）*/

/* 寄存器语义常量 */
#define MAX7219_SPI_DIGIT_COUNT         (8u)     /* 8 个 digit 寄存器 */
#define MAX7219_SPI_BYTES_PER_CHIP      (2u)     /* 每次寄存器写 = 地址 + 数据 */
#define MAX7219_SPI_INTENSITY_MAX       (15u)    /* 亮度 0 到 15 */
#define MAX7219_SPI_SCAN_LIMIT_MIN      (1u)     /* 有效扫描位数 1 到 8 */
#define MAX7219_SPI_SCAN_LIMIT_MAX      (8u)     /* 寄存器值 = 位数 - 1，即 0x00 到 0x07 */
#define MAX7219_SPI_DECODE_NONE         (0x00u)  /* 不译码（原始段码） */
/* MAX7219 的 0x0C：1 = 正常工作，0 = 关断（与 enable = 1 表示关断的直觉相反，故取反）*/
#define MAX7219_SPI_SHUTDOWN_NORMAL     (0x01u)  /* 正常工作（点亮显示）*/
#define MAX7219_SPI_SHUTDOWN_ENTER      (0x00u)  /* 进入关断（关闭显示）*/
#define MAX7219_SPI_DISPLAY_TEST_OFF    (0x00u)  /* 0 = 正常显示；1 = 全部 LED 强制点亮，仅产线测试用 */
#define MAX7219_SPI_DEFAULT_INTENSITY   (8u)     /* 上电默认亮度：中等 */
#define MAX7219_SPI_DEFAULT_TIMEOUT_MS  (100u)   /* config.timeoutMs 传 0 时采用的超时 */

/* 级联片数区间：至少一片；上限 64 用于拦截 2 * chainLength 的误传 */
#define MAX7219_SPI_MIN_CHAIN_LENGTH    (1u)
#define MAX7219_SPI_MAX_CHAIN_LENGTH    (64u)

/* ==================================================================
 * 接口函数前置声明：装配函数表时按 Max7219Interface 字段顺序排列
 * ================================================================== */
static int  max7219SpiInit(void* instance);
static int  max7219SpiShutdown(void* instance, int enable);
static int  max7219SpiSetIntensity(void* instance, uint8_t intensity);
static int  max7219SpiSetScanLimit(void* instance, uint8_t digits);
static int  max7219SpiSetDecodeMode(void* instance, uint8_t mode);
static int  max7219SpiClear(void* instance);
static int  max7219SpiWriteRegister(void* instance, uint32_t chipIndex, uint8_t reg, uint8_t value);
static int  max7219SpiWriteFrame(void* instance, const uint8_t* frame, uint32_t len);
static void max7219SpiDeleteInstance(void* instance);

/* ==================================================================
 * 内部工具
 * ================================================================== */

/* 整帧广播：把 (reg, value) 填满整帧后一次下发。
 * 链上每片收到的都是同一条「地址 + 数据」，即一次整帧广播。
 * 下发后 frameBuffer 内保存的就是刚发出的内容，可直接复用。 */
static int max7219SpiBroadcast(Max7219Spi* self, uint8_t reg, uint8_t value) {
    uint32_t index = 0;
    uint32_t byteCount = 0;

    if (NULL == self || NULL == self->spi || NULL == self->frameBuffer) {
        return MAX7219_SPI_ERR_PARAM;
    }

    byteCount = MAX7219_SPI_BYTES_PER_CHIP * self->chainLength;

    /* 每一对字节都是同一个（地址, 数据）：级联链上任一片都命中目标寄存器 */
    for (index = 0; index < byteCount; index += MAX7219_SPI_BYTES_PER_CHIP) {
        self->frameBuffer[index]     = reg;
        self->frameBuffer[index + 1] = value;
    }

    return max7219SpiWriteFrame((void*)self, self->frameBuffer, byteCount);
}

/* ==================================================================
 * 接口实现
 * ================================================================== */

/* 上电初始化：依次下发上电默认配置，与位带实现保持一致：
 *   ① shutdown(0)     —— 退出关断，进入正常显示（0x0C = 0x01）
 *   ② displayTest 0   —— 关测试模式（0x0F = 0x00）
 *   ③ decodeMode 0x00 —— 不译码（原始段码，0x09）
 *   ④ scanLimit 8     —— 8 位扫描（0x0B = 0x07）
 *   ⑤ intensity 8     —— 中等亮度（0x0A = 0x08）
 *   ⑥ clear           —— 8 轮整帧广播清屏（0x01 到 0x08 写 0x00）
 * 注意顺序：MAX7219 在 Shutdown 模式下会忽略 digits 与部分控制寄存器的写入，
 * 因此必须【先】写 0x0C=0x01 退出关断，【再】写 0x0F=0 关 Display Test，
 * 否则关测试模式的写入可能被吞掉，表现为上电后 LED 全亮且清屏无效。
 * 第 ② 步不可省略：若 0x0F 被噪声或 0xFF 帧误置为 1，芯片进入测试模式，
 * 所有 LED 强制点亮且忽略 digit 数据，任何清屏都无效。
 * 任一步失败立即返回，不继续下发（避免半配置状态）。
 * 本函数不触碰片选以外的总线细节，也不做速率协商（归调用方的 Spi 具象）。 */
static int max7219SpiInit(void* instance) {
    Max7219Spi* self = (Max7219Spi*)instance;
    int result = MAX7219_SPI_OK;

    if (NULL == self || NULL == self->spi || NULL == self->frameBuffer) {
        return MAX7219_SPI_ERR_PARAM;
    }

    /* ① 退出关断：0x0C = 0x01 才是正常工作，必须最先写，后续寄存器写才会生效 */
    result = max7219SpiShutdown((void*)self, 0);
    if (MAX7219_SPI_OK != result) {
        return result;
    }
    /* ② 关测试模式：0x0F = 0x00，仅产线测试才置 1（放在退出关断之后才不会被吞掉） */
    result = max7219SpiBroadcast(self, (uint8_t)MAX7219_SPI_REG_DISPLAY_TEST,
                                 (uint8_t)MAX7219_SPI_DISPLAY_TEST_OFF);
    if (MAX7219_SPI_OK != result) {
        return result;
    }
    result = max7219SpiSetDecodeMode((void*)self, (uint8_t)MAX7219_SPI_DECODE_NONE);
    if (MAX7219_SPI_OK != result) {
        return result;
    }
    result = max7219SpiSetScanLimit((void*)self, (uint8_t)MAX7219_SPI_SCAN_LIMIT_MAX);
    if (MAX7219_SPI_OK != result) {
        return result;
    }
    result = max7219SpiSetIntensity((void*)self, (uint8_t)MAX7219_SPI_DEFAULT_INTENSITY);
    if (MAX7219_SPI_OK != result) {
        return result;
    }

    return max7219SpiClear((void*)self);
}

/* shutdown：enable 非 0 -> 进入关断（0x0C 写 0x00）；为 0 -> 正常工作（0x0C 写 0x01）；
 * MAX7219 的 0x0C 是 bit0 = 1 表示正常工作、bit0 = 0 表示关断，
 * 与「enable = 1 表示关断」的直觉相反，故此处显式取反。广播到链上所有片。 */
static int max7219SpiShutdown(void* instance, int enable) {
    Max7219Spi* self = (Max7219Spi*)instance;
    uint8_t value = (0 != enable) ? (uint8_t)MAX7219_SPI_SHUTDOWN_ENTER
                                  : (uint8_t)MAX7219_SPI_SHUTDOWN_NORMAL;

    if (NULL == self) {
        return MAX7219_SPI_ERR_PARAM;
    }

    return max7219SpiBroadcast(self, (uint8_t)MAX7219_SPI_REG_SHUTDOWN, value);
}

/* 亮度：0 到 15，广播到链上所有片 */
static int max7219SpiSetIntensity(void* instance, uint8_t intensity) {
    Max7219Spi* self = (Max7219Spi*)instance;

    if (NULL == self) {
        return MAX7219_SPI_ERR_PARAM;
    }
    if ((uint32_t)intensity > MAX7219_SPI_INTENSITY_MAX) {
        return MAX7219_SPI_ERR_PARAM;
    }

    return max7219SpiBroadcast(self, (uint8_t)MAX7219_SPI_REG_INTENSITY, intensity);
}

/* 有效扫描位数：抽象层传 1 到 8，寄存器实际写 位数 - 1（0x00 到 0x07） */
static int max7219SpiSetScanLimit(void* instance, uint8_t digits) {
    Max7219Spi* self = (Max7219Spi*)instance;

    if (NULL == self) {
        return MAX7219_SPI_ERR_PARAM;
    }
    if ((uint32_t)digits < MAX7219_SPI_SCAN_LIMIT_MIN ||
        (uint32_t)digits > MAX7219_SPI_SCAN_LIMIT_MAX) {
        return MAX7219_SPI_ERR_PARAM;
    }

    /* 位数 1 到 8 已在前一步限定，故 digits - 1 必落在 0x00 到 0x07 */
    return max7219SpiBroadcast(self, (uint8_t)MAX7219_SPI_REG_SCAN_LIMIT,
                               (uint8_t)(digits - 1u));
}

/* 译码模式：8 位位图任意取值均合法，不做范围校验，广播到所有片 */
static int max7219SpiSetDecodeMode(void* instance, uint8_t mode) {
    Max7219Spi* self = (Max7219Spi*)instance;

    if (NULL == self) {
        return MAX7219_SPI_ERR_PARAM;
    }

    return max7219SpiBroadcast(self, (uint8_t)MAX7219_SPI_REG_DECODE_MODE, mode);
}

/* 清屏：8 个 digit 寄存器逐个「整帧广播」清零。
 * 每轮把整条链的 (DIGITn, 0x00) 一次性移出并锁存，共 8 轮；
 * 不逐字节拼帧，也不逐片单独下发。 */
static int max7219SpiClear(void* instance) {
    Max7219Spi* self = (Max7219Spi*)instance;
    uint32_t digit = 0;
    int result = MAX7219_SPI_OK;

    if (NULL == self || NULL == self->frameBuffer || NULL == self->spi) {
        return MAX7219_SPI_ERR_PARAM;
    }

    for (digit = 0; digit < MAX7219_SPI_DIGIT_COUNT; ++digit) {
        result = max7219SpiBroadcast(self,
                                     (uint8_t)(MAX7219_SPI_REG_DIGIT0 + digit),
                                     0x00u);
        if (MAX7219_SPI_OK != result) {
            return result;
        }
    }

    return MAX7219_SPI_OK;
}

/* 单寄存器写：只剩目标片有内容，其余片填 NOOP（0x00, 0x00）。
 *
 * 级联偏移计算（本模块最容易写错的一处）：
 *   frame 按「先发最远端」排列，先移出的字节被推到链尾，
 *   故 chipIndex = 0（直接接 DIN 的第一片）的数据落在帧的最后两个字节：
 *       offset = 2 * (chainLength - 1 - chipIndex)
 *   校验链：chipIndex = chainLength - 1（最远端）-> offset = 0（帧首）
 *           chipIndex = 0（最近端）             -> offset = 2 * chainLength - 2（帧尾）
 */
static int max7219SpiWriteRegister(void* instance, uint32_t chipIndex,
                                   uint8_t reg, uint8_t value) {
    Max7219Spi* self = (Max7219Spi*)instance;
    uint32_t byteCount = 0;
    uint32_t offset = 0;

    if (NULL == self || NULL == self->spi || NULL == self->frameBuffer) {
        return MAX7219_SPI_ERR_PARAM;
    }
    if (chipIndex >= self->chainLength) {
        return MAX7219_SPI_ERR_PARAM;
    }

    byteCount = MAX7219_SPI_BYTES_PER_CHIP * self->chainLength;
    memset(self->frameBuffer, 0, (size_t)byteCount);   /* 其余片 = NOOP(0x00) + 0x00 */

    offset = MAX7219_SPI_BYTES_PER_CHIP * (self->chainLength - 1u - chipIndex);
    self->frameBuffer[offset]     = reg;
    self->frameBuffer[offset + 1] = value;

    return max7219SpiWriteFrame((void*)self, self->frameBuffer, byteCount);
}

/* 整帧下发：校验 len 必须等于 2 * chainLength 且为偶数，然后
 *   ① SpiSelect(1)：CS 拉低，开启整帧移位窗口
 *   ② SpiTransfer(frame, NULL, len, timeoutMs)：单向整帧移出
 *   ③ SpiSelect(0)：CS 拉高锁存 —— 无论成败都必须释放片选
 *
 * 结果归一（优先级自上而下）：
 *   传输返回负值            -> 该负值原样上抛
 *   传输返回非负但不等于 len -> -1（短传输/截断，视为链路或参数异常）
 *   片选释放返回非 0        -> 该值上抛（此时数据已发出，仅提示总线状态异常）
 * frame 允许与本实例的 frameBuffer 指向同一块内存（内部调用即如此）。
 */
static int max7219SpiWriteFrame(void* instance, const uint8_t* frame, uint32_t len) {
    Max7219Spi* self = (Max7219Spi*)instance;
    uint32_t byteCount = 0;
    int result = MAX7219_SPI_OK;
    int transferResult = 0;
    int releaseResult = 0;

    if (NULL == self || NULL == self->spi) {
        return MAX7219_SPI_ERR_PARAM;
    }
    if (NULL == frame) {
        return MAX7219_SPI_ERR_PARAM;
    }
    if (0u == len || 0u != (len % MAX7219_SPI_BYTES_PER_CHIP)) {
        return MAX7219_SPI_ERR_PARAM;
    }

    byteCount = MAX7219_SPI_BYTES_PER_CHIP * self->chainLength;
    if (len != byteCount) {
        return MAX7219_SPI_ERR_PARAM;   /* 帧长必须与级联片数严格匹配 */
    }

    /* ① 选中：CS 拉低，开启整帧移位窗口 */
    result = SpiSelect(self->spi, 1);
    if (MAX7219_SPI_OK != result) {
        /* 选中失败也尽力回到未选中态，避免总线被长期占用 */
        (void)SpiSelect(self->spi, 0);
        return result;
    }

    /* ② 整帧移出：单向写，接收缓冲为 NULL，超时用实例保存值 */
    transferResult = SpiTransfer(self->spi, frame, NULL, byteCount, self->timeoutMs);

    /* ③ 无论成功还是失败，都必须释放片选（CS 回到高电平锁存既发数据） */
    releaseResult = SpiSelect(self->spi, 0);

    /* ④ 结果归一：传输错误优先于片选释放错误 */
    if (transferResult < 0) {
        return transferResult;
    }
    if ((int)byteCount != transferResult) {
        return MAX7219_SPI_ERR_PARAM;
    }
    if (MAX7219_SPI_OK != releaseResult) {
        return releaseResult;
    }

    return MAX7219_SPI_OK;
}

/* 析构：只释放本模块拥有的资源。
 * 【禁止】在此调用 DeleteSpi —— spi 是外部注入、所有权在调用方。 */
static void max7219SpiDeleteInstance(void* instance) {
    Max7219Spi* self = (Max7219Spi*)instance;

    if (NULL == self) {
        return;
    }

    if (NULL != self->frameBuffer) {
        if (NULL != max7219Free) {
            max7219Free(self->frameBuffer);
        }
        self->frameBuffer = NULL;   /* 释放后置空，防悬空 */
    }

    /* 断开对外部总线的引用，但不销毁对方 */
    self->spi = NULL;

    if (NULL != max7219Free) {
        max7219Free(self);
    }
    /* self 是局部副本，置空无意义；调用方持有的是 Max7219 抽象对象 */
}

/* ==================================================================
 * 虚表集中返回：字段顺序严格遵循 Max7219Interface
 * ================================================================== */
Max7219Interface Max7219SpiInterface(void) {
    _RETURN_MAX7219_INTERFACE_WITH(max7219SpiInit,
                                   max7219SpiShutdown,
                                   max7219SpiSetIntensity,
                                   max7219SpiSetScanLimit,
                                   max7219SpiSetDecodeMode,
                                   max7219SpiClear,
                                   max7219SpiWriteRegister,
                                   max7219SpiWriteFrame,
                                   max7219SpiDeleteInstance);
}

/* ==================================================================
 * 具体构造
 * ================================================================== */
Max7219* NewMax7219Spi(const Max7219SpiConfig* config) {
    Max7219Spi* ins = NULL;
    Max7219* max7219 = NULL;
    uint32_t byteCount = 0;

    /* ---------- ① 入参校验 ---------- */
    if (NULL == config) {
        return NULL;
    }
    if (NULL == config->spi) {
        return NULL;                       /* 依赖注入缺失，无法下发任何字节 */
    }
    if (NULL == config->spi->instance) {
        return NULL;                       /* 抽象层句柄未装配，调用虚表必然失败 */
    }
    if (config->chainLength < MAX7219_SPI_MIN_CHAIN_LENGTH ||
        config->chainLength > MAX7219_SPI_MAX_CHAIN_LENGTH) {
        return NULL;                       /* 至少一片，且拦截 2 * chainLength 的误传 */
    }
    if (NULL == max7219Malloc || NULL == max7219Free) {
        return NULL;
    }

    /* ---------- ② 分配具体实例 ---------- */
    ins = (Max7219Spi*)max7219Malloc(sizeof(Max7219Spi));
    if (NULL == ins) {
        return NULL;
    }
    memset(ins, 0, sizeof(*ins));          /* spi / frameBuffer 一并归零 */

    /* ---------- ③ 分配整帧缓冲（失败回滚实例） ---------- */
    byteCount = MAX7219_SPI_BYTES_PER_CHIP * config->chainLength;
    ins->frameBuffer = (uint8_t*)max7219Malloc((size_t)byteCount);
    if (NULL == ins->frameBuffer) {
        max7219Free(ins);
        return NULL;
    }
    memset(ins->frameBuffer, 0, (size_t)byteCount);

    /* ---------- ④ 拷贝配置（构造函数不操作总线，上电配置在 init 内） ---------- */
    ins->spi         = config->spi;
    ins->chainLength = config->chainLength;
    /* 超时 0 归一为默认值，避免把 0 传到最底层被解释成「非阻塞」 */
    ins->timeoutMs   = (0u == config->timeoutMs)
                       ? (uint32_t)MAX7219_SPI_DEFAULT_TIMEOUT_MS
                       : config->timeoutMs;

    /* ---------- ⑤ 装配抽象父类：禁止手工赋值 instance / interfaces ---------- */
    max7219 = NewMax7219((void*)ins, Max7219SpiInterface());
    if (NULL == max7219) {
        /* 回滚：先释放本模块拥有的缓冲，再释放具体实例 */
        max7219Free(ins->frameBuffer);
        ins->frameBuffer = NULL;
        max7219Free(ins);
        return NULL;
    }

    return max7219;
}
