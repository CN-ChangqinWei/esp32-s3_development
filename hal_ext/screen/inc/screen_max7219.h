#ifndef _SCREEN_MAX7219_H
#define _SCREEN_MAX7219_H

/* ==================================================================
 * screen_max7219.h —— Screen 抽象父类（screen.h）的具象派生类：
 *                     MAX7219 级联点阵屏实现（可复用层，平台无关）。
 *
 * 结构（OOP in C）：
 *   父类  Screen        = { void* instance; ScreenInterface interfaces; }
 *   本类  ScreenMax7219 = 注入的驱动抽象 + 点阵几何 + 像素内存
 *
 *   NewScreenMax7219() 内部创建 ScreenMax7219 具体实例，再调用
 *   NewScreen(ins, ScreenMax7219Interface()) 完成抽象装配；
 *   使用方只面向 Screen* 抽象编程，全程不接触具体类型。
 *
 * 平台无关性（本模块的立身之本）：
 *   1. 只依赖抽象父类 screen.h 与驱动抽象 max7219.h，
 *      禁止包含 driver、freertos 下的平台头与 esp_*.h。
 *   2. 硬件访问一律经 Max7219* 抽象走虚表，不直接碰寄存器、不碰引脚。
 *   3. 内存一律走 screenMalloc / screenFree（可在外部整体替换）。
 *
 * 几何约定（片索引、帧格式的完整推导见 screen_max7219.c 文件头）：
 *   width  = chipsX * blockSize
 *   height = chipsY * blockSize
 *   原点在左上角，x 向右递增、y 向下递增，合法范围 x 属于 [0, width)、
 *   y 属于 [0, height)。
 *   片索引按下标行主序：chipIndex = chipY * chipsX + chipX，
 *   其中 chipIndex = 0 是直接接 DIN 的第一片（离 MCU 最近）。
 *   全局坐标 (x, y) 落在片 (chipX = x 除以 blockSize,
 *   chipY = y 除以 blockSize)，片内坐标为两轴各自取模的结果。
 *
 * 所有权约定：
 *   Max7219* 驱动由调用方创建并拥有，本模块只借用，
 *   析构时【不】调用 DeleteMax7219。
 *   pixels 像素内存由本模块在构造时经 screenMalloc 分配，
 *   析构时经 screenFree 释放。
 *
 * 返回值约定（与抽象父类 screen.h 保持一致）：
 *    0   成功
 *   -1   参数非法（instance 为空、配置非法、坐标越界）
 *   其余负值为驱动抽象 Max7219* 上抛的错误码，原样透传不二次编码。
 * ================================================================== */

#include <stdint.h>

#include "screen.h"
#include "max7219.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------
 * 构造配置：外部依赖注入
 *
 *   driver    : MAX7219 驱动抽象对象；为空则构造失败
 *   blockSize : 每片边长（像素），MAX7219 固定为 8；合法范围 1 到 8
 *   chipsX    : 横向片数，必须大于等于 1
 *   chipsY    : 纵向片数，必须大于等于 1
 *   invertDigit : digit 数据极性，0 正相 / 1 反相，非 0 一律按 1 处理
 *   级联总片数 chipsX 乘 chipsY 不得大于 64
 * ------------------------------------------------------------------ */
typedef struct ScreenMax7219Config {
    Max7219* driver;      /* 注入的驱动抽象，本模块不销毁 */
    uint32_t blockSize;   /* 每片边长，MAX7219 = 8 */
    uint32_t chipsX;      /* 横向片数 */
    uint32_t chipsY;      /* 纵向片数 */
    /* digit 数据极性：0 = 正相（digit 位 1 = 点亮，MAX7219 手册默认行为）
     *                 1 = 反相（digit 位 0 = 点亮，部分点阵模块如此）
     * 只影响「像素 -> hardware digit 字节」的打包，不影响帧内存「1 = 亮」的语义。 */
    uint8_t invertDigit;
} ScreenMax7219Config;

/* ------------------------------------------------------------------
 * 具象数据成员：只允许中性类型（uint32_t / uint8_t / 抽象指针），
 * 不出现任何平台句柄、引脚号与 SPI 设备结构。
 *
 *   pixels : width 乘 height 字节的像素缓冲，1 字节 1 像素，
 *            非 0 视为点亮，归本实例所有。
 *            clear / fill / setPixel 只改这块内存，
 *            真正下发硬件由 flush 完成。
 * ------------------------------------------------------------------ */
typedef struct ScreenMax7219 {
    Max7219* driver;     /* 外部注入，本模块不销毁 */
    uint32_t blockSize;  /* 每片边长（像素），MAX7219 为 8 */
    uint32_t chipsX;     /* 横向片数 */
    uint32_t chipsY;     /* 纵向片数 */
    uint8_t  invertDigit;/* digit 极性：0 正相 / 1 反相，构造时归一化为 0 或 1 */
    uint32_t width;      /* 像素宽 = chipsX * blockSize */
    uint32_t height;     /* 像素高 = chipsY * blockSize */
    uint8_t* pixels;     /* width * height 字节，非 0 即点亮 */
} ScreenMax7219;

/* ------------------------------------------------------------------
 * 具体构造：校验配置 -> 分配具象实例 -> 分配 pixels
 *           -> 拷贝字段 -> 内部调用 NewScreen() 完成抽象装配。
 *
 * 本函数【不】操作硬件（不下发寄存器、不清屏），
 * 上电配置由后续的 ScreenInit() 完成。
 *
 * 返回：装配好的 Screen* 抽象对象；
 *       config 为空、driver 为空、blockSize 不在 1 到 8、
 *       chipsX 或 chipsY 为 0、级联片数大于 64、
 *       内存不足或抽象构造失败时返回 NULL（失败路径全部回滚，不泄漏）。
 * ------------------------------------------------------------------ */
Screen* NewScreenMax7219(const ScreenMax7219Config* config);

/* ------------------------------------------------------------------
 * 虚表集中返回：字段顺序严格遵循 ScreenInterface 定义
 * 供 NewScreen 装配与「外部实例注入」场景复用。
 * ------------------------------------------------------------------ */
ScreenInterface ScreenMax7219Interface(void);

#ifdef __cplusplus
}
#endif

#endif /* _SCREEN_MAX7219_H */
