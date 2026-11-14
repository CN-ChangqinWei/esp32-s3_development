/**
 * @file screen_max7219.c
 * @brief Screen 具象派生类实现：MAX7219 级联点阵屏（可复用层，平台无关）。
 *
 * 职责边界：
 *   ① NewScreenMax7219()：校验配置 -> 分配 ScreenMax7219 -> 分配 pixels
 *      -> 拷贝字段 -> 调用 NewScreen() 装配抽象父类（失败全回滚）
 *   ② 接口函数全部 static，首参恒为 void* instance，首行取具体实例 self
 *   ③ 硬件访问一律经 Max7219* 抽象，不碰任何寄存器 / 引脚
 *   ④ 内存一律走 screenMalloc / screenFree（可在外部整体替换）
 *
 * 硬约束：本文件不含任何平台代码，只 include 标准头与三份抽象头，
 *         禁止引入 ESP-IDF 与 FreeRTOS 的平台头。
 *
 * 返回值约定（与 screen.h 一致）：
 *    0   成功
 *   -1   参数非法（instance 为空、几何字段非法、坐标越界）
 *   驱动抽象 Max7219* 上抛的负值原样透传，不二次编码。
 *
 * ==================================================================
 * 几何与帧格式（本模块最容易写错的地方，集中说明）
 * ==================================================================
 * ① 屏幕尺寸：
 *      width  = chipsX * blockSize
 *      height = chipsY * blockSize
 *    原点在左上角，x 向右递增，y 向下递增。
 *
 * ② 片索引：按下标行主序，chipY 向下增：
 *      chipIndex = chipY * chipsX + chipX
 *    其中 chipIndex = 0 是直接接 DIN 的第一片（离 MCU 最近的那片）。
 *
 * ③ 坐标分解：全局坐标 (x, y) 对应
 *      片 = (chipX = x 整除 blockSize, chipY = y 整除 blockSize)
 *      片内 = (x 对 blockSize 取模, y 对 blockSize 取模)
 *
 * ④ 寄存器映射：一个 digit 寄存器承载片内一行：
 *      reg  = 0x01 + row                       （DIGIT0 到 DIGIT7）
 *      data 的 bit (blockSize - 1 - col) 对应该行第 col 列，
 *      即 col = 0 为最左列，col = blockSize - 1 为最右列。
 *      这与 MAX7219 段码的 MSB 在左一致：左移后高位即左侧像素。
 *
 * ⑤ 整帧下发：一次 Max7219WriteFrame 只能写「所有片的同一个 digit 寄存器」，
 *    因此 flush 需要 blockSize 轮整帧：
 *      第 r 轮把【每片第 r 行】塞进同一帧，
 *      帧长 = 2 * chainLength（chainLength = chipsX * chipsY，每片 2 字节）。
 *    帧内偏移遵循 MAX7219 抽象层「先发最远端」的约定：
 *      offset = 2 * (chainLength - 1 - chipIndex)
 *    校验链：chipIndex = chainLength - 1（最远端）-> offset = 0（帧首）
 *           chipIndex = 0（最近端）            -> offset = 帧尾两个字节
 * ==================================================================
 */

#include "screen_max7219.h"

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "screen.h"
#include "max7219.h"

/* ==================================================================
 * 内部常量
 * ================================================================== */

/* 返回值（与抽象父类数值保持一致） */
#define SCREEN_MAX7219_OK        (SCREEN_OK)
#define SCREEN_MAX7219_ERR_PARAM (SCREEN_ERR_PARAM)

/* MAX7219 寄存器地址：digit 寄存器从 0x01 起，DIGIT0 到 DIGIT7 依次加 1 */
#define SCREEN_MAX7219_REG_DIGIT0   (0x01u)

/* 每次寄存器写占 2 字节：地址 + 数据 */
#define SCREEN_MAX7219_BYTES_PER_CHIP (2u)

/* 每片边长上限：MAX7219 一片就是 8 乘 8 点阵 */
#define SCREEN_MAX7219_BLOCK_SIZE_MIN (1u)
#define SCREEN_MAX7219_BLOCK_SIZE_MAX (8u)

/* 级联总片数上限：拦截误传的几何参数与整帧缓冲的尺寸失控 */
#define SCREEN_MAX7219_CHIP_COUNT_MIN (1u)
#define SCREEN_MAX7219_CHIP_COUNT_MAX (64u)

/* 清屏后 init 使用的默认亮度（0 到 15，取中等值） */
#define SCREEN_MAX7219_DEFAULT_INTENSITY (8u)

/* 整帧缓冲上限字节数 = 2 * 最大级联片数；flush 用栈上定长缓冲，
 * 免去每次刷屏的动态分配与分配失败分支 */
#define SCREEN_MAX7219_FRAME_BYTES \
    (SCREEN_MAX7219_CHIP_COUNT_MAX * SCREEN_MAX7219_BYTES_PER_CHIP)

/* ==================================================================
 * 接口函数前置声明：装配函数表时按 ScreenInterface 字段顺序排列
 * ================================================================== */
static int  screenMax7219Init(void* instance);
static int  screenMax7219Deinit(void* instance);
static int  screenMax7219Clear(void* instance);
static int  screenMax7219Fill(void* instance, int on);
static int  screenMax7219SetPixel(void* instance, int32_t x, int32_t y, int on);
static int  screenMax7219GetPixel(void* instance, int32_t x, int32_t y);
static int  screenMax7219Flush(void* instance);
static int  screenMax7219GetWidth(void* instance);
static int  screenMax7219GetHeight(void* instance);
static void screenMax7219DeleteInstance(void* instance);

/* ==================================================================
 * 内部工具
 * ================================================================== */

/* 像素总数：width 乘 height；几何在构造时已校验，故可安全相乘 */
static uint32_t screenMax7219PixelCount(const ScreenMax7219* self) {
    if (NULL == self) {
        return 0u;
    }
    return self->width * self->height;
}

/* 坐标归一化：非 0 一律视为点亮，落到 1 字节像素值 0 或 1 */
static uint8_t screenMax7219NormalizeOn(int on) {
    return (0 != on) ? (uint8_t)1u : (uint8_t)0u;
}

/* 把某一片的某一行打包成一个 digit 寄存器值。
 *   y = chipY * blockSize + row  （全局行）
 *   x = chipX * blockSize + col  （全局列）
 *   col = 0 为最左列，落在 data 的 bit(blockSize - 1 - col)
 * 数据极性由构造参数 invertDigit 决定，本函数不再硬编码任何模块特性。
 * blockSize 已在构造时限定为 1 到 8，故移位量恒在 0 到 7，不会越界。 */
static uint8_t screenMax7219PackRow(const ScreenMax7219* self,
                                    uint32_t chipX, uint32_t chipY, uint32_t row) {
    uint8_t  data = 0u;
    uint32_t col  = 0u;
    uint32_t x    = 0u;
    uint32_t y    = 0u;

    if (NULL == self || NULL == self->pixels) {
        return 0u;
    }

    y = chipY * self->blockSize + row;
    if (y >= self->height) {
        return 0u;
    }

    for (col = 0u; col < self->blockSize; ++col) {
        x = chipX * self->blockSize + col;
        if (x >= self->width) {
            continue;
        }
        if (0u != self->pixels[y * self->width + x]) {
            data = (uint8_t)(data | (uint8_t)(1u << (self->blockSize - 1u - col)));
        }
    }

    /* 极性由 invertDigit 决定（构造参数注入，默认 0 为正相）：
     *   正相：保持 data 原样，digit 位 1 点亮，符合 MAX7219 手册默认行为；
     *   反相：按位取反后下发，digit 位 0 才点亮，适配段输出反相的点阵模块。
     * 帧内存统一保持「1 = 亮」的语义（SetPixel / Fill / DrawBitmap 都按此约定），
     * 只在打包成硬件 digit 字节的这一刻按极性处理，硬件差异不外泄到上层：
     * 上层语义、GetPixel 回读、各 MatrixPattern 图案均无需改动。
     * blockSize 小于 8 时，未使用的低位在反相下为 1，即「熄灭」，同样符合预期。 */
    return (0u != self->invertDigit) ? (uint8_t)(~data) : data;
}

/* 把内存像素表清空，不动硬件（clear / deinit 共用） */
static int screenMax7219WipeMemory(ScreenMax7219* self) {
    uint32_t pixelCount = 0u;

    if (NULL == self || NULL == self->pixels) {
        return SCREEN_MAX7219_ERR_PARAM;
    }

    pixelCount = screenMax7219PixelCount(self);
    if (0u != pixelCount) {
        memset(self->pixels, 0, (size_t)pixelCount);
    }
    return SCREEN_MAX7219_OK;
}

/* ==================================================================
 * 接口实现
 * ================================================================== */

/* 上电初始化：先配好驱动（初始化 / 亮度），再让内存帧与硬件同时熄屏。
 * 顺序固定：驱动未配置成功就不动像素表；任一步失败立即返回其负值。 */
static int screenMax7219Init(void* instance) {
    ScreenMax7219* self = (ScreenMax7219*)instance;
    int result = SCREEN_MAX7219_OK;

    if (NULL == self || NULL == self->driver || NULL == self->pixels) {
        return SCREEN_MAX7219_ERR_PARAM;
    }

    /* ① 驱动抽象自行完成上电默认配置（退出 shutdown、不译码、8 位扫描等） */
    result = Max7219Init(self->driver);
    if (SCREEN_MAX7219_OK != result) {
        return result;
    }
    /* ② 亮度统一取中等值 8 */
    result = Max7219SetIntensity(self->driver, (uint8_t)SCREEN_MAX7219_DEFAULT_INTENSITY);
    if (SCREEN_MAX7219_OK != result) {
        return result;
    }
    /* ③ 硬件显示寄存器归零：不经 Max7219Clear 直接写 0x00，
     *    而是走「内存清空 + 本层 flush」，由 pack 按 invertDigit 产出对应的
     *    「全灭」字节（正相 0x00、反相 0xFF），两种极性都正确熄屏。
     *    这样也保证 init 结束时内存帧与硬件一致。 */
    result = screenMax7219WipeMemory(self);
    if (SCREEN_MAX7219_OK != result) {
        return result;
    }
    result = screenMax7219Flush(self);
    if (SCREEN_MAX7219_OK != result) {
        return result;
    }
    return SCREEN_MAX7219_OK;      /* 原来第 ④ 步 wipe 已合并到这里，去掉重复调用 */
}

/* 反初始化：内存帧与硬件显示一起熄屏，但【不】销毁注入的驱动
 * （所有权始终在调用方，复位后可再次 init 复用同一实例）。 */
static int screenMax7219Deinit(void* instance) {
    ScreenMax7219* self = (ScreenMax7219*)instance;
    int result = SCREEN_MAX7219_OK;

    if (NULL == self || NULL == self->driver) {
        return SCREEN_MAX7219_ERR_PARAM;
    }

    /* ① 内存帧清零，后续 flush 读到的就是「全灭」 */
    result = screenMax7219WipeMemory(self);
    if (SCREEN_MAX7219_OK != result) {
        return result;
    }

    /* ② 经本层 flush 把「全灭」下发到硬件：由 pack 按 invertDigit 决定实际
     *    字节（正相 0x00、反相 0xFF），释放前把面板熄掉，两种极性均适用。 */
    return screenMax7219Flush(self);
}

/* 全屏熄灭：只改内存，不自动 flush（由调用方显式 ScreenFlush） */
static int screenMax7219Clear(void* instance) {
    ScreenMax7219* self = (ScreenMax7219*)instance;

    if (NULL == self) {
        return SCREEN_MAX7219_ERR_PARAM;
    }

    return screenMax7219WipeMemory(self);
}

/* 全屏点亮 / 熄灭：只改内存，不自动 flush（由调用方显式 ScreenFlush） */
static int screenMax7219Fill(void* instance, int on) {
    ScreenMax7219* self = (ScreenMax7219*)instance;
    uint32_t pixelCount = 0u;
    uint8_t  value = 0u;

    if (NULL == self || NULL == self->pixels) {
        return SCREEN_MAX7219_ERR_PARAM;
    }

    pixelCount = screenMax7219PixelCount(self);
    if (0u == pixelCount) {
        return SCREEN_MAX7219_ERR_PARAM;
    }

    /* 点亮写 0xFF（任何非 0 都算点亮），熄灭写 0x00 */
    value = (0 != on) ? (uint8_t)0xFFu : (uint8_t)0x00u;
    memset(self->pixels, (int)value, (size_t)pixelCount);

    return SCREEN_MAX7219_OK;
}

/* 单点写入：坐标越界返回 -1；on 归一化为 0 或 1 后落内存。
 * 坐标校验用无符号比较，顺带拦截负数入参。 */
static int screenMax7219SetPixel(void* instance, int32_t x, int32_t y, int on) {
    ScreenMax7219* self = (ScreenMax7219*)instance;

    if (NULL == self || NULL == self->pixels) {
        return SCREEN_MAX7219_ERR_PARAM;
    }
    if (x < 0 || y < 0) {
        return SCREEN_MAX7219_ERR_PARAM;
    }
    if ((uint32_t)x >= self->width || (uint32_t)y >= self->height) {
        return SCREEN_MAX7219_ERR_PARAM;
    }

    self->pixels[(uint32_t)y * self->width + (uint32_t)x] = screenMax7219NormalizeOn(on);
    return SCREEN_MAX7219_OK;
}

/* 单点读取：坐标越界返回 -1；成功返回 1 表示亮、0 表示灭 */
static int screenMax7219GetPixel(void* instance, int32_t x, int32_t y) {
    ScreenMax7219* self = (ScreenMax7219*)instance;

    if (NULL == self || NULL == self->pixels) {
        return SCREEN_MAX7219_ERR_PARAM;
    }
    if (x < 0 || y < 0) {
        return SCREEN_MAX7219_ERR_PARAM;
    }
    if ((uint32_t)x >= self->width || (uint32_t)y >= self->height) {
        return SCREEN_MAX7219_ERR_PARAM;
    }

    return (0u != self->pixels[(uint32_t)y * self->width + (uint32_t)x]) ? 1 : 0;
}

/* 刷屏：把内存像素表按「所有片同一行」组帧，共 blockSize 轮整帧下发。
 *
 *   第 r 轮：
 *     chainLength = chipsX * chipsY
 *     frameLen    = 2 * chainLength
 *     对每一片 (chipX, chipY)：
 *       chipIndex      = chipY * chipsX + chipX
 *       offset         = 2 * (chainLength - 1 - chipIndex)
 *       frame[offset]     = 0x01 + r            （DIGIT0 起，第 r 行）
 *       frame[offset + 1] = 该片第 r 行的 8 个像素位
 *   任一帧失败立即返回驱动的负值，不继续下发（避免半屏新旧混显）。
 *
 * 帧缓冲用栈上定长数组：上限 2 * 64 = 128 字节，远小于任一任务的栈余量，
 * 且省掉刷屏路径上的动态分配与分配失败分支。 */
static int screenMax7219Flush(void* instance) {
    ScreenMax7219* self  = (ScreenMax7219*)instance;
    uint8_t  frame[SCREEN_MAX7219_FRAME_BYTES];
    uint32_t chainLength = 0u;
    uint32_t frameLen    = 0u;
    uint32_t row         = 0u;
    uint32_t chipX       = 0u;
    uint32_t chipY       = 0u;
    uint32_t chipIndex   = 0u;
    uint32_t offset      = 0u;
    int result = SCREEN_MAX7219_OK;

    if (NULL == self || NULL == self->driver || NULL == self->pixels) {
        return SCREEN_MAX7219_ERR_PARAM;
    }
    if (0u == self->chipsX || 0u == self->chipsY) {
        return SCREEN_MAX7219_ERR_PARAM;
    }

    chainLength = self->chipsX * self->chipsY;
    if (chainLength < SCREEN_MAX7219_CHIP_COUNT_MIN ||
        chainLength > SCREEN_MAX7219_CHIP_COUNT_MAX) {
        return SCREEN_MAX7219_ERR_PARAM;
    }
    frameLen = SCREEN_MAX7219_BYTES_PER_CHIP * chainLength;
    if (frameLen > (uint32_t)SCREEN_MAX7219_FRAME_BYTES) {
        return SCREEN_MAX7219_ERR_PARAM;
    }

    for (row = 0u; row < self->blockSize; ++row) {
        /* 每片每轮都写满自己的两个字节，先归零只是防御性兜底 */
        memset(frame, 0, (size_t)frameLen);

        for (chipY = 0u; chipY < self->chipsY; ++chipY) {
            for (chipX = 0u; chipX < self->chipsX; ++chipX) {
                chipIndex = chipY * self->chipsX + chipX;
                /* 先发最远端：chipIndex 越大越靠帧首 */
                offset = SCREEN_MAX7219_BYTES_PER_CHIP *
                         (chainLength - 1u - chipIndex);

                frame[offset]     = (uint8_t)(SCREEN_MAX7219_REG_DIGIT0 + row);
                frame[offset + 1] = screenMax7219PackRow(self, chipX, chipY, row);
            }
        }

        result = Max7219WriteFrame(self->driver, frame, frameLen);
        if (SCREEN_MAX7219_OK != result) {
            return result;
        }
    }

    return SCREEN_MAX7219_OK;
}

/* 像素宽；构造时已保证大于等于 1 */
static int screenMax7219GetWidth(void* instance) {
    ScreenMax7219* self = (ScreenMax7219*)instance;

    if (NULL == self) {
        return SCREEN_MAX7219_ERR_PARAM;
    }

    return (int)self->width;
}

/* 像素高；构造时已保证大于等于 1 */
static int screenMax7219GetHeight(void* instance) {
    ScreenMax7219* self = (ScreenMax7219*)instance;

    if (NULL == self) {
        return SCREEN_MAX7219_ERR_PARAM;
    }

    return (int)self->height;
}

/* 析构：只释放本模块拥有的资源。
 * 【禁止】在此调用 DeleteMax7219 —— driver 是外部注入、所有权在调用方。 */
static void screenMax7219DeleteInstance(void* instance) {
    ScreenMax7219* self = (ScreenMax7219*)instance;

    if (NULL == self) {
        return;
    }

    if (NULL != self->pixels) {
        if (NULL != screenFree) {
            screenFree(self->pixels);
        }
        self->pixels = NULL;   /* 释放后置空，防悬空 */
    }

    /* 断开对外部对象的引用，但不销毁对方 */
    self->driver = NULL;

    if (NULL != screenFree) {
        screenFree(self);
    }
    /* self 是局部副本，置空无意义；调用方持有的是 Screen 抽象对象 */
}

/* ==================================================================
 * 虚表集中返回：字段顺序严格遵循 ScreenInterface
 * ================================================================== */
ScreenInterface ScreenMax7219Interface(void) {
    _RETURN_SCREEN_INTERFACE_WITH(screenMax7219Init,
                                  screenMax7219Deinit,
                                  screenMax7219Clear,
                                  screenMax7219Fill,
                                  screenMax7219SetPixel,
                                  screenMax7219GetPixel,
                                  screenMax7219Flush,
                                  screenMax7219GetWidth,
                                  screenMax7219GetHeight,
                                  screenMax7219DeleteInstance);
}

/* ==================================================================
 * 具体构造
 * ================================================================== */
Screen* NewScreenMax7219(const ScreenMax7219Config* config) {
    ScreenMax7219* ins         = NULL;
    Screen*        screen      = NULL;
    uint32_t       chainLength = 0u;
    uint32_t       pixelCount  = 0u;

    /* ---------- ① 配置校验 ---------- */
    if (NULL == config) {
        return NULL;
    }
    if (NULL == config->driver) {
        return NULL;                       /* 依赖注入缺失，无法产生任何波形 */
    }
    if (config->blockSize < SCREEN_MAX7219_BLOCK_SIZE_MIN ||
        config->blockSize > SCREEN_MAX7219_BLOCK_SIZE_MAX) {
        return NULL;                       /* 每片边长必须是 1 到 8 */
    }
    if (config->chipsX < SCREEN_MAX7219_CHIP_COUNT_MIN ||
        config->chipsY < SCREEN_MAX7219_CHIP_COUNT_MIN) {
        return NULL;                       /* 横纵至少各一片 */
    }
    /* 先各自挡上限再相乘：单轴都不超过 64，故乘积不会 32 位溢出 */
    if (config->chipsX > SCREEN_MAX7219_CHIP_COUNT_MAX ||
        config->chipsY > SCREEN_MAX7219_CHIP_COUNT_MAX) {
        return NULL;
    }
    chainLength = config->chipsX * config->chipsY;
    if (chainLength < SCREEN_MAX7219_CHIP_COUNT_MIN ||
        chainLength > SCREEN_MAX7219_CHIP_COUNT_MAX) {
        return NULL;                       /* 级联总片数不得大于 64 */
    }
    if (NULL == screenMalloc || NULL == screenFree) {
        return NULL;                       /* 分配钩子被外部置空，无法构造 */
    }

    /* ---------- ② 分配具体实例 ---------- */
    ins = (ScreenMax7219*)screenMalloc(sizeof(ScreenMax7219));
    if (NULL == ins) {
        return NULL;
    }
    memset(ins, 0, sizeof(*ins));          /* pixels / driver / 几何一并归零 */

    /* ---------- ③ 分配像素缓冲（失败回滚实例） ---------- */
    /* 尺寸上限：64 片 * 8 = 512，故 width 乘 height 最大 512 * 512，无溢出 */
    pixelCount = (config->chipsX * config->blockSize) *
                 (config->chipsY * config->blockSize);
    if (0u == pixelCount) {
        screenFree(ins);
        return NULL;
    }

    ins->pixels = (uint8_t*)screenMalloc((size_t)pixelCount);
    if (NULL == ins->pixels) {
        screenFree(ins);
        return NULL;
    }
    memset(ins->pixels, 0, (size_t)pixelCount);

    /* ---------- ④ 拷贝配置（构造函数不操作硬件，上电配置在 init 内） ---------- */
    ins->driver    = config->driver;
    ins->blockSize = config->blockSize;
    ins->chipsX    = config->chipsX;
    ins->chipsY    = config->chipsY;
    /* 极性归一化为 0 或 1：非 0 即反相，容忍调用方传入任意非零值 */
    ins->invertDigit = (0u != config->invertDigit) ? (uint8_t)1u : (uint8_t)0u;
    ins->width     = config->chipsX * config->blockSize;
    ins->height    = config->chipsY * config->blockSize;

    /* ---------- ⑤ 装配抽象父类：禁止手工赋值 instance / interfaces ---------- */
    screen = NewScreen((void*)ins, ScreenMax7219Interface());
    if (NULL == screen) {
        /* 回滚：先释放本模块拥有的像素缓冲，再释放具体实例 */
        screenFree(ins->pixels);
        ins->pixels = NULL;
        screenFree(ins);
        return NULL;
    }

    return screen;
}
