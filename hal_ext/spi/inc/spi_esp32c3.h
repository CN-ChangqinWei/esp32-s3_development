#ifndef _SPI_ESP32C3_H
#define _SPI_ESP32C3_H

/* ==================================================================
 * spi_esp32c3.h —— SPI 具象派生类（OOP in C）：ESP32-C3
 *
 * 结构：
 *   父类  Spi          = { void* instance; SpiInterface interfaces; }
 *   本类  SpiEsp32c3   = 引脚 + 句柄 + 运行期配置 + 状态标志
 *   NewSpiEsp32c3() 内部创建 SpiEsp32c3 实例，并调用 NewSpi(ins, 虚表) 装配，
 *   使用方只面向 Spi* 抽象编程（全程不接触 ESP-IDF 类型）。
 *
 * 硬约束（本头文件的立身之本）：
 *   1. 禁止包含任何 ESP-IDF / FreeRTOS 头（driver 与 freertos 下的头文件、esp_*.h）；
 *      平台类型（spi_device_handle_t 等）只允许出现在 .c 内部。
 *   2. 设备句柄用 void* deviceHandle 承载 spi_device_handle_t；
 *      其余字段只用 uint32_t / int32_t / uint8_t。
 *   3. 片选由具象层手动控制（driver 侧 spics_io_num = -1），
 *      因此 SpiSelect() 的"选中/未选中"语义与硬件 CS 电平严格一致。
 *
 * 返回值约定（与抽象层一致）：
 *    0   成功
 *   -1   参数非法 / 平台调用失败
 *   -2   select() 专用：本实例无片选引脚（csPin 传入 -1）
 * ================================================================== */

#include "spi.h"

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------
 * 返回值约定宏
 * ------------------------------------------------------------------ */
#define SPI_ESP32C3_OK         (0)
#define SPI_ESP32C3_ERR_PARAM  (-1)
#define SPI_ESP32C3_ERR_NO_CS  (-2)   /* select()：无片选引脚 */

/* ------------------------------------------------------------------
 * 具体实例结构体：ESP32-C3 通用 SPI（SPI2_HOST）
 *
 * 字段全部为标量或 void*，确保本头文件可被纯 C / C++ 使用方安全包含。
 * ------------------------------------------------------------------ */
typedef struct SpiEsp32c3 {
    /* ---------- 硬件资源：构造时确定，运行期不变 ---------- */
    int32_t sclkPin;        /* 已归一化的 GPIO 号，-1 表示未使用 */
    int32_t mosiPin;        /* 同上 */
    int32_t misoPin;        /* 同上 */
    int32_t csPin;          /* 同上；< 0 表示无片选 */
    void*   deviceHandle;   /* 承载 spi_device_handle_t，禁止在本头文件展开 */

    /* ---------- 运行期配置：setConfig 可能更新 ---------- */
    uint32_t clockHz;       /* 时钟频率，恒 > 0 */
    uint8_t  mode;          /* SpiMode0..SpiMode3，直接映射 IDF mode 0..3 */
    uint8_t  bitsPerWord;    /* 语义字段，0 已归一化为 8 */

    /* ---------- 状态标志：部分初始化失败时避免重复释放 ---------- */
    uint8_t busInitialized;  /* 1 = spi_bus_initialize 成功且未释放 */
    uint8_t csReady;         /* 1 = 片选 GPIO 已配置为输出 */
} SpiEsp32c3;

/* ------------------------------------------------------------------
 * 具体构造：建立总线 + 挂载设备，并在内部调用 NewSpi() 完成装配
 *
 *   sclkPin / mosiPin / misoPin / csPin : GPIO 号；传 -1 表示该脚未使用
 *                                        （sclkPin 必须有效）
 *   csPin 传 -1 : 无片选，SpiSelect() 返回 -2
 *   config 为 NULL : 使用 SpiDefaultConfig(1000000)，即 mode0 / 8bit / 1MHz
 *
 * 返回：装配好的 Spi* 抽象对象；失败（引脚非法 / 内存不足 / 平台调用失败）返回 NULL
 * ------------------------------------------------------------------ */
Spi* NewSpiEsp32c3(uint32_t sclkPin, uint32_t mosiPin, uint32_t misoPin,
                   uint32_t csPin, const SpiConfig* config);

/* ------------------------------------------------------------------
 * 虚表集中返回：字段顺序严格遵循 SpiInterface 定义
 * ------------------------------------------------------------------ */
SpiInterface SpiEsp32c3Interface(void);

#ifdef __cplusplus
}
#endif

#endif /* _SPI_ESP32C3_H */
