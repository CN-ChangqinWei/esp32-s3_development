/**
 * @file max7219_bitbang.c
 * @brief MAX7219 具象派生类实现：3 线软件位带（Bit-Bang），平台无关。
 *
 * 职责边界：
 *   ① NewMax7219BitBang()：校验入参 -> 分配 Max7219BitBang -> 分配整帧缓冲
 *      -> 拷贝配置 -> 调用 NewMax7219() 装配抽象父类（失败全回滚）
 *   ② 接口函数全部 static，首参恒为 void* instance，首行取具体实例 self
 *   ③ 引脚操作一律经 GPIO 抽象（GpioInit / GpioWrite），不碰任何寄存器
 *   ④ 内存一律走 max7219Malloc / max7219Free（可在外部整体替换）
 *
 * 硬约束：本文件不含任何平台代码，只 include <stdint.h> / <stdlib.h> /
 *         <string.h> 与三份抽象头，禁止引入 ESP-IDF 与 FreeRTOS 的平台头。
 *
 * 返回值约定（与 max7219.h 一致）：
 *    0  成功
 *   -1  参数非法 / chipIndex 越界 / len 与链长不匹配
 *   -2  未实现（九槽位全实现，本模块不主动返回；出现即底层 Gpio 上抛）
 *   底层 GpioInit / GpioWrite 失败时其负值原样上抛。
 */

#include "max7219_bitbang.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "max7219.h"
#include "gpio.h"

/* ==================================================================
 * 内部常量
 * ================================================================== */

/* MAX7219 寄存器地址 */
#define MAX7219_BITBANG_REG_NOOP        (0x00u)   /* 空操作，用于填充非目标片 */
#define MAX7219_BITBANG_REG_DIGIT0      (0x01u)   /* DIGIT0，DIGIT1..DIGIT7 依次 +1 */
#define MAX7219_BITBANG_REG_DECODE_MODE (0x09u)
#define MAX7219_BITBANG_REG_INTENSITY   (0x0Au)
#define MAX7219_BITBANG_REG_SCAN_LIMIT  (0x0Bu)
#define MAX7219_BITBANG_REG_SHUTDOWN    (0x0Cu)
#define MAX7219_BITBANG_REG_DISPLAY_TEST (0x0Fu) /* Display Test（地址 0x0F）*/

/* 寄存器语义常量 */
#define MAX7219_BITBANG_DIGIT_COUNT     (8u)      /* 8 个 digit 寄存器 */
#define MAX7219_BITBANG_BYTES_PER_CHIP  (2u)      /* 每次寄存器写 = 地址 + 数据 */
#define MAX7219_BITBANG_INTENSITY_MAX   (15u)     /* 亮度 0 到 15 */
#define MAX7219_BITBANG_SCAN_LIMIT_MIN  (1u)      /* 有效扫描位数 1 到 8 */
#define MAX7219_BITBANG_SCAN_LIMIT_MAX  (8u)      /* 寄存器值 = 位数 - 1，即 0x00 到 0x07 */
#define MAX7219_BITBANG_DECODE_NONE     (0x00u)   /* 不译码（原始段码） */
/* MAX7219 的 0x0C：1 = 正常工作，0 = 关断（与 enable = 1 表示关断的直觉相反，故取反）*/
#define MAX7219_BITBANG_SHUTDOWN_NORMAL (0x01u)   /* 正常工作（点亮显示）*/
#define MAX7219_BITBANG_SHUTDOWN_ENTER  (0x00u)   /* 进入关断（关闭显示）*/
#define MAX7219_BITBANG_DISPLAY_TEST_OFF (0x00u)  /* 0 = 正常显示；1 = 全部 LED 强制点亮，仅产线测试用 */
#define MAX7219_BITBANG_DEFAULT_INTENSITY (8u)    /* 上电默认亮度：中等 */

/* 级联片数上限：物理上远达不到，仅用于拦截 2 * chainLength 的溢出与误传 */
#define MAX7219_BITBANG_MAX_CHAIN_LENGTH (1024u)

/* 每个电平边沿之后的额外延时（微秒）：MAX7219 只需 50ns 级保持时间，
 * 1us 对任何主频都足够安全；未注入 delayUs 时本延时整体跳过。 */
#define MAX7219_BITBANG_EDGE_DELAY_US   (1u)

/* ==================================================================
 * 接口函数前置声明：装配函数表时按 Max7219Interface 字段顺序排列
 * ================================================================== */
static int  max7219BitBangInit(void* instance);
static int  max7219BitBangShutdown(void* instance, int enable);
static int  max7219BitBangSetIntensity(void* instance, uint8_t intensity);
static int  max7219BitBangSetScanLimit(void* instance, uint8_t digits);
static int  max7219BitBangSetDecodeMode(void* instance, uint8_t mode);
static int  max7219BitBangClear(void* instance);
static int  max7219BitBangWriteRegister(void* instance, uint32_t chipIndex, uint8_t reg, uint8_t value);
static int  max7219BitBangWriteFrame(void* instance, const uint8_t* frame, uint32_t len);
static void max7219BitBangDeleteInstance(void* instance);

/* ==================================================================
 * 内部工具
 * ================================================================== */

/* 边沿延时：delayUs 由调用方注入，为空表示不额外延时 */
static void max7219BitBangDelayEdge(const Max7219BitBang* self) {
    if (NULL == self) {
        return;
    }
    if (NULL != self->delayUs) {
        self->delayUs(MAX7219_BITBANG_EDGE_DELAY_US);
    }
}

/* 写一个引脚电平，成功后立即做边沿延时。
 * 底层失败时原样上抛其负值（不二次编码，避免掩盖真实原因）。 */
static int max7219BitBangWritePin(Max7219BitBang* self, uint32_t pin, GpioLevel level) {
    int result = 0;

    if (NULL == self || NULL == self->gpio) {
        return MAX7219_BITBANG_ERR_PARAM;
    }

    result = GpioWrite(self->gpio, pin, level);
    if (GPIO_OK != result) {
        return result;
    }

    max7219BitBangDelayEdge(self);
    return MAX7219_BITBANG_OK;
}

/* 移出一个字节：MSB first（最高位先行）
 * 时序：DIN 置位 -> CLK 上升沿（MAX7219 在上升沿采样 DIN）-> CLK 拉低
 * 每个电平变化后都调用一次 delayUs（若已注入）。
 */
static int max7219BitBangSendByte(Max7219BitBang* self, uint8_t value) {
    uint32_t bitIndex = 0;
    int result = MAX7219_BITBANG_OK;

    if (NULL == self) {
        return MAX7219_BITBANG_ERR_PARAM;
    }

    for (bitIndex = 0; bitIndex < 8u; ++bitIndex) {
        /* 0x80 >> bitIndex 依次取 bit7 ... bit0，即 MSB first */
        GpioLevel level = (0 != (value & (uint8_t)(0x80u >> bitIndex)))
                          ? GpioLevelHigh : GpioLevelLow;

        /* ① DIN 先稳定 */
        result = max7219BitBangWritePin(self, self->dinPin, level);
        if (MAX7219_BITBANG_OK != result) {
            return result;
        }
        /* ② CLK 上升沿：此刻 DIN 被移入移位寄存器 */
        result = max7219BitBangWritePin(self, self->clkPin, GpioLevelHigh);
        if (MAX7219_BITBANG_OK != result) {
            return result;
        }
        /* ③ CLK 回低，为下一位准备 */
        result = max7219BitBangWritePin(self, self->clkPin, GpioLevelLow);
        if (MAX7219_BITBANG_OK != result) {
            return result;
        }
    }

    return MAX7219_BITBANG_OK;
}

/* 广播一次寄存器写：把 (reg, value) 填满整帧后一次下发，
 * 链上所有片都会收到同一条「地址 + 数据」，即一次整帧广播。 */
static int max7219BitBangBroadcast(Max7219BitBang* self, uint8_t reg, uint8_t value) {
    uint32_t index = 0;
    uint32_t byteCount = 0;

    if (NULL == self || NULL == self->frameBuffer || NULL == self->gpio) {
        return MAX7219_BITBANG_ERR_PARAM;
    }

    byteCount = MAX7219_BITBANG_BYTES_PER_CHIP * self->chainLength;

    /* 每一对字节都是同一个 (地址, 数据)：级联链上任一片都命中目标寄存器 */
    for (index = 0; index < byteCount; index += MAX7219_BITBANG_BYTES_PER_CHIP) {
        self->frameBuffer[index]     = reg;
        self->frameBuffer[index + 1] = value;
    }

    return max7219BitBangWriteFrame((void*)self, self->frameBuffer, byteCount);
}

/* ==================================================================
 * 接口实现
 * ================================================================== */

/* 上电初始化：
 *   ① 三脚配置为推挽输出、无上下拉
 *   ② DIN / CLK / LOAD 全部先置低（CLK 空闲低，LOAD 低电平开启移位窗口）
 *   ③ 下发上电默认配置，顺序固定（与 SPI 具象保持一致）：
 *      1) shutdown 0     —— 退出关断，进入正常显示（0x0C = 0x01）
 *      2) displayTest 0  —— 关测试模式（0x0F = 0x00）
 *      3) decodeMode 0x00 —— 不译码（原始段码，0x09）
 *      4) scanLimit 8    —— 8 位扫描（0x0B = 0x07）
 *      5) intensity 8    —— 中等亮度（0x0A = 0x08）
 *      6) clear          —— 8 轮整帧广播清屏（0x01 到 0x08 写 0x00）
 * 注意顺序：MAX7219 在 Shutdown 模式下会忽略 digits 与部分控制寄存器的写入，
 * 因此必须【先】写 0x0C=0x01 退出关断，【再】写 0x0F=0 关 Display Test，
 * 否则关测试模式的写入可能被吞掉，表现为上电后 LED 全亮且清屏无效。
 * 第 2) 步不可省略：若 0x0F 被噪声或 0xFF 帧误置为 1，芯片进入测试模式，
 * 所有 LED 强制点亮且忽略 digit 数据，任何清屏都无效。
 *   任一步失败立即返回，不继续下发（避免半配置状态）。
 */
static int max7219BitBangInit(void* instance) {
    Max7219BitBang* self = (Max7219BitBang*)instance;
    int result = MAX7219_BITBANG_OK;

    if (NULL == self || NULL == self->gpio || NULL == self->frameBuffer) {
        return MAX7219_BITBANG_ERR_PARAM;
    }

    /* ① 三个引脚统一配置：输出 / 无上下拉 */
    result = GpioInit(self->gpio, self->dinPin, GpioDirOutput, GpioPullNone);
    if (GPIO_OK != result) {
        return result;
    }
    result = GpioInit(self->gpio, self->clkPin, GpioDirOutput, GpioPullNone);
    if (GPIO_OK != result) {
        return result;
    }
    result = GpioInit(self->gpio, self->loadPin, GpioDirOutput, GpioPullNone);
    if (GPIO_OK != result) {
        return result;
    }

    /* ② 空闲电平：DIN 低（未选中数据）、CLK 低（无时钟沿）、LOAD 低（移位窗口） */
    result = max7219BitBangWritePin(self, self->dinPin, GpioLevelLow);
    if (MAX7219_BITBANG_OK != result) {
        return result;
    }
    result = max7219BitBangWritePin(self, self->clkPin, GpioLevelLow);
    if (MAX7219_BITBANG_OK != result) {
        return result;
    }
    result = max7219BitBangWritePin(self, self->loadPin, GpioLevelLow);
    if (MAX7219_BITBANG_OK != result) {
        return result;
    }

    /* ③ 退出关断：0x0C = 0x01 才是正常工作，必须最先写，后续寄存器写才会生效 */
    result = max7219BitBangShutdown((void*)self, 0);
    if (MAX7219_BITBANG_OK != result) {
        return result;
    }
    /* ④ 关测试模式：0x0F = 0x00，仅产线测试才置 1（放在退出关断之后才不会被吞掉） */
    result = max7219BitBangBroadcast(self, (uint8_t)MAX7219_BITBANG_REG_DISPLAY_TEST,
                                     (uint8_t)MAX7219_BITBANG_DISPLAY_TEST_OFF);
    if (MAX7219_BITBANG_OK != result) {
        return result;
    }
    result = max7219BitBangSetDecodeMode((void*)self, MAX7219_BITBANG_DECODE_NONE);
    if (MAX7219_BITBANG_OK != result) {
        return result;
    }
    result = max7219BitBangSetScanLimit((void*)self, (uint8_t)MAX7219_BITBANG_SCAN_LIMIT_MAX);
    if (MAX7219_BITBANG_OK != result) {
        return result;
    }
    result = max7219BitBangSetIntensity((void*)self, (uint8_t)MAX7219_BITBANG_DEFAULT_INTENSITY);
    if (MAX7219_BITBANG_OK != result) {
        return result;
    }
    result = max7219BitBangClear((void*)self);

    return result;
}

/* shutdown：enable 非 0 -> 进入关断（0x0C 写 0x00）；为 0 -> 正常工作（0x0C 写 0x01）；
 * MAX7219 的 0x0C 是 bit0 = 1 表示正常工作、bit0 = 0 表示关断，
 * 与「enable = 1 表示关断」的直觉相反，故此处显式取反。广播到链上所有片。 */
static int max7219BitBangShutdown(void* instance, int enable) {
    Max7219BitBang* self = (Max7219BitBang*)instance;
    uint8_t value = (0 != enable) ? (uint8_t)MAX7219_BITBANG_SHUTDOWN_ENTER
                                  : (uint8_t)MAX7219_BITBANG_SHUTDOWN_NORMAL;

    if (NULL == self) {
        return MAX7219_BITBANG_ERR_PARAM;
    }

    return max7219BitBangBroadcast(self, (uint8_t)MAX7219_BITBANG_REG_SHUTDOWN, value);
}

/* 亮度：0 到 15，广播到链上所有片 */
static int max7219BitBangSetIntensity(void* instance, uint8_t intensity) {
    Max7219BitBang* self = (Max7219BitBang*)instance;

    if (NULL == self) {
        return MAX7219_BITBANG_ERR_PARAM;
    }
    if ((uint32_t)intensity > MAX7219_BITBANG_INTENSITY_MAX) {
        return MAX7219_BITBANG_ERR_PARAM;
    }

    return max7219BitBangBroadcast(self, (uint8_t)MAX7219_BITBANG_REG_INTENSITY, intensity);
}

/* 有效扫描位数：抽象层传 1 到 8，寄存器实际写 位数 - 1（0x00 到 0x07） */
static int max7219BitBangSetScanLimit(void* instance, uint8_t digits) {
    Max7219BitBang* self = (Max7219BitBang*)instance;

    if (NULL == self) {
        return MAX7219_BITBANG_ERR_PARAM;
    }
    if ((uint32_t)digits < MAX7219_BITBANG_SCAN_LIMIT_MIN ||
        (uint32_t)digits > MAX7219_BITBANG_SCAN_LIMIT_MAX) {
        return MAX7219_BITBANG_ERR_PARAM;
    }

    /* 位数 1 到 8 已在前一步限定，故 digits - 1 必落在 0x00 到 0x07 */
    return max7219BitBangBroadcast(self, (uint8_t)MAX7219_BITBANG_REG_SCAN_LIMIT,
                                   (uint8_t)(digits - 1u));
}

/* 译码模式：8 位位图任意取值均合法，不做范围校验，广播到所有片 */
static int max7219BitBangSetDecodeMode(void* instance, uint8_t mode) {
    Max7219BitBang* self = (Max7219BitBang*)instance;

    if (NULL == self) {
        return MAX7219_BITBANG_ERR_PARAM;
    }

    return max7219BitBangBroadcast(self, (uint8_t)MAX7219_BITBANG_REG_DECODE_MODE, mode);
}

/* 清屏：8 个 digit 寄存器逐个「整帧广播」清零。
 * 每轮把整条链的 (DIGITn, 0x00) 一次性移出 + 一次 LOAD 锁存，
 * 共 8 轮锁存；不逐字节拼 16 次，也不逐片单独下发。 */
static int max7219BitBangClear(void* instance) {
    Max7219BitBang* self = (Max7219BitBang*)instance;
    uint32_t digit = 0;
    int result = MAX7219_BITBANG_OK;

    if (NULL == self || NULL == self->frameBuffer || NULL == self->gpio) {
        return MAX7219_BITBANG_ERR_PARAM;
    }

    for (digit = 0; digit < MAX7219_BITBANG_DIGIT_COUNT; ++digit) {
        result = max7219BitBangBroadcast(self,
                                        (uint8_t)(MAX7219_BITBANG_REG_DIGIT0 + digit),
                                        0x00u);
        if (MAX7219_BITBANG_OK != result) {
            return result;
        }
    }

    return MAX7219_BITBANG_OK;
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
static int max7219BitBangWriteRegister(void* instance, uint32_t chipIndex,
                                       uint8_t reg, uint8_t value) {
    Max7219BitBang* self = (Max7219BitBang*)instance;
    uint32_t byteCount = 0;
    uint32_t offset = 0;

    if (NULL == self || NULL == self->frameBuffer || NULL == self->gpio) {
        return MAX7219_BITBANG_ERR_PARAM;
    }
    if (chipIndex >= self->chainLength) {
        return MAX7219_BITBANG_ERR_PARAM;
    }

    byteCount = MAX7219_BITBANG_BYTES_PER_CHIP * self->chainLength;
    memset(self->frameBuffer, 0, (size_t)byteCount);   /* 其余片 = NOOP(0x00) + 0x00 */

    offset = MAX7219_BITBANG_BYTES_PER_CHIP * (self->chainLength - 1u - chipIndex);
    self->frameBuffer[offset]             = reg;
    self->frameBuffer[offset + 1]         = value;

    return max7219BitBangWriteFrame((void*)self, self->frameBuffer, byteCount);
}

/* 整帧下发：校验 len 必须等于 2 * chainLength 且为偶数，
 * 然后 LOAD 拉低 -> 逐字节 MSB first 移出 -> LOAD 拉高锁存 -> LOAD 拉低。
 * frame 允许与本实例的 frameBuffer 指向同一块内存（内部调用即如此）。 */
static int max7219BitBangWriteFrame(void* instance, const uint8_t* frame, uint32_t len) {
    Max7219BitBang* self = (Max7219BitBang*)instance;
    uint32_t byteCount = 0;
    uint32_t index = 0;
    int result = MAX7219_BITBANG_OK;

    if (NULL == self || NULL == self->gpio) {
        return MAX7219_BITBANG_ERR_PARAM;
    }
    if (NULL == frame) {
        return MAX7219_BITBANG_ERR_PARAM;
    }
    if (0u == len || 0u != (len % MAX7219_BITBANG_BYTES_PER_CHIP)) {
        return MAX7219_BITBANG_ERR_PARAM;
    }

    byteCount = MAX7219_BITBANG_BYTES_PER_CHIP * self->chainLength;
    if (len != byteCount) {
        return MAX7219_BITBANG_ERR_PARAM;   /* 帧长必须与级联片数严格匹配 */
    }

    /* ① LOAD 拉低：开启整帧移位窗口 */
    result = max7219BitBangWritePin(self, self->loadPin, GpioLevelLow);
    if (MAX7219_BITBANG_OK != result) {
        return result;
    }

    /* ② 逐字节移出，字节内 MSB first，字节间按帧数组顺序（先最远端） */
    for (index = 0; index < byteCount; ++index) {
        result = max7219BitBangSendByte(self, frame[index]);
        if (MAX7219_BITBANG_OK != result) {
            /* 中途失败：尽力把 LOAD 拉低回到空闲态，再上抛错误 */
            (void)max7219BitBangWritePin(self, self->loadPin, GpioLevelLow);
            return result;
        }
    }

    /* ③ LOAD 上升沿锁存已移入的整帧 */
    result = max7219BitBangWritePin(self, self->loadPin, GpioLevelHigh);
    if (MAX7219_BITBANG_OK != result) {
        return result;
    }

    /* ④ LOAD 回低，回到空闲态（下一次写帧从低电平开始） */
    return max7219BitBangWritePin(self, self->loadPin, GpioLevelLow);
}

/* 析构：只释放本模块拥有的资源。
 * 【禁止】在此调用 DeleteGpio —— gpio 是外部注入、所有权在调用方。 */
static void max7219BitBangDeleteInstance(void* instance) {
    Max7219BitBang* self = (Max7219BitBang*)instance;

    if (NULL == self) {
        return;
    }

    if (NULL != self->frameBuffer) {
        if (NULL != max7219Free) {
            max7219Free(self->frameBuffer);
        }
        self->frameBuffer = NULL;   /* 释放后置空，防悬空 */
    }

    /* 断开对外部对象的引用，但不销毁对方 */
    self->gpio    = NULL;
    self->delayUs = NULL;

    if (NULL != max7219Free) {
        max7219Free(self);
    }
    /* self 是局部副本，置空无意义；调用方持有的是 Max7219 抽象对象 */
}

/* ==================================================================
 * 虚表集中返回：字段顺序严格遵循 Max7219Interface
 * ================================================================== */
Max7219Interface Max7219BitBangInterface(void) {
    _RETURN_MAX7219_INTERFACE_WITH(max7219BitBangInit,
                                   max7219BitBangShutdown,
                                   max7219BitBangSetIntensity,
                                   max7219BitBangSetScanLimit,
                                   max7219BitBangSetDecodeMode,
                                   max7219BitBangClear,
                                   max7219BitBangWriteRegister,
                                   max7219BitBangWriteFrame,
                                   max7219BitBangDeleteInstance);
}

/* ==================================================================
 * 具体构造
 * ================================================================== */
Max7219* NewMax7219BitBang(const Max7219BitBangConfig* config) {
    Max7219BitBang* ins = NULL;
    Max7219* max7219 = NULL;
    uint32_t byteCount = 0;

    /* ---------- ① 入参校验 ---------- */
    if (NULL == config) {
        return NULL;
    }
    if (NULL == config->gpio) {
        return NULL;                       /* 依赖注入缺失，无法产生任何波形 */
    }
    if (0u == config->chainLength) {
        return NULL;                       /* 至少一片 */
    }
    if (config->chainLength > MAX7219_BITBANG_MAX_CHAIN_LENGTH) {
        return NULL;                       /* 拦截 2 * chainLength 溢出与误传 */
    }
    /* DIN / CLK / LOAD 必须三个互不相同的引脚 */
    if (config->dinPin == config->clkPin ||
        config->dinPin == config->loadPin ||
        config->clkPin == config->loadPin) {
        return NULL;
    }
    if (NULL == max7219Malloc || NULL == max7219Free) {
        return NULL;
    }

    /* ---------- ② 分配具体实例 ---------- */
    ins = (Max7219BitBang*)max7219Malloc(sizeof(Max7219BitBang));
    if (NULL == ins) {
        return NULL;
    }
    memset(ins, 0, sizeof(*ins));          /* frameBuffer / gpio / delayUs 一并归零 */

    /* ---------- ③ 分配整帧缓冲（失败回滚实例） ---------- */
    byteCount = MAX7219_BITBANG_BYTES_PER_CHIP * config->chainLength;
    ins->frameBuffer = (uint8_t*)max7219Malloc((size_t)byteCount);
    if (NULL == ins->frameBuffer) {
        max7219Free(ins);
        return NULL;
    }
    memset(ins->frameBuffer, 0, (size_t)byteCount);

    /* ---------- ④ 拷贝配置（构造函数不操作硬件，上电配置在 init 内） ---------- */
    ins->gpio        = config->gpio;
    ins->dinPin      = config->dinPin;
    ins->clkPin      = config->clkPin;
    ins->loadPin     = config->loadPin;
    ins->chainLength = config->chainLength;
    ins->delayUs     = config->delayUs;

    /* ---------- ⑤ 装配抽象父类：禁止手工赋值 instance / interfaces ---------- */
    max7219 = NewMax7219((void*)ins, Max7219BitBangInterface());
    if (NULL == max7219) {
        /* 回滚：先释放本模块拥有的缓冲，再释放具体实例 */
        max7219Free(ins->frameBuffer);
        ins->frameBuffer = NULL;
        max7219Free(ins);
        return NULL;
    }

    return max7219;
}
