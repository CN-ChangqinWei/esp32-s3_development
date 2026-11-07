#ifndef _ADC_ESP32C3_H
#define _ADC_ESP32C3_H

/* ==========================================================================
 * adc_esp32c3.h —— ADC 抽象父类（adc.h）的 ESP32-C3 具象派生类
 *
 * OOP in C 关系：
 *   抽象父类 Adc（adc.h）
 *       { void* instance; AdcInterface interfaces; }
 *                │ instance
 *                ▼
 *   具象派生类 AdcEsp32c3（本文件）
 *       { void* unitHandle; void* caliHandle; uint32_t unit; uint8_t bitWidth; … }
 *
 *   上层（main / service）只依赖 adc.h 的 Adc*，经 AdcReadRaw / AdcReadMilliVolts
 *   等包装函数调用；运行时实际执行 adc_esp32c3.c 内的静态接口实现。
 *
 * 头文件约束（强制）：
 *   1. 本文件【禁止】包含任何 ESP-IDF / FreeRTOS 头（esp_adc/*、esp_*.h、
 *      driver/*、freertos/*），保证使用方不被拖入平台头文件链；
 *   2. 结构体字段只用 uint32_t / uint8_t / void*（平台句柄一律以 void* 承载），
 *   3. adc_oneshot_unit_handle_t、adc_cali_handle_t、adc_atten_t 等平台类型
 *      只允许出现在 adc_esp32c3.c 内部。
 *
 * 平台映射（ESP32-C3，SAR ADC one-shot + curve fitting 校准）：
 *   单元号 1（ADC_UNIT_1）：通道 0..4  <-> GPIO0..GPIO4
 *   单元号 2（ADC_UNIT_2）：通道 0     <-> GPIO5
 *
 *   AdcAtten0dB   -> ADC_ATTEN_DB_0
 *   AdcAtten2_5dB -> ADC_ATTEN_DB_2_5
 *   AdcAtten6dB   -> ADC_ATTEN_DB_6
 *   AdcAtten12dB  -> ADC_ATTEN_DB_12
 *
 * 返回值约定（与 adc.h 一致，-3 为具象层自定义）：
 *    0   成功
 *   -1   参数非法（空指针 / unit 越界 / 位宽越界 / 通道越界 / 单元未初始化）
 *   -2   未实现 / 不支持：本实现用于 readMilliVolts「校准不可用」
 *   -3   ESP-IDF 驱动（esp_err_t）返回错误
 * ========================================================================== */

#include <stdint.h>

#include "adc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * ESP32-C3 平台常量（编译期常量，不做任何运行时强约束之外的假设）
 * -------------------------------------------------------------------------- */
#define ADC_ESP32C3_UNIT_1              (1U)   /* ADC1 */
#define ADC_ESP32C3_UNIT_2              (2U)   /* ADC2 */

#define ADC_ESP32C3_MIN_BITWIDTH        (9U)
#define ADC_ESP32C3_MAX_BITWIDTH        (12U)  /* 等于 SOC_ADC_RTC_MAX_BITWIDTH */
#define ADC_ESP32C3_DEFAULT_BITWIDTH    (12U)  /* 构造时传 0 按此值处理 */

#define ADC_ESP32C3_ADC1_MAX_CHANNEL    (4U)   /* ADC1：CH0..CH4 <-> GPIO0..GPIO4 */
#define ADC_ESP32C3_ADC2_MAX_CHANNEL    (0U)   /* ADC2：CH0     <-> GPIO5 */

/* 衰减 / 就绪状态按通道号索引的槽位数：覆盖 adc_channel_t 的 0..9 */
#define ADC_ESP32C3_CHANNEL_SLOT_COUNT  (10U)

/* 具象实现私有返回码：校准不可用（复用抽象层 -2 语义）、驱动失败 */
#define ADC_ESP32C3_ERR_NO_CALIBRATION  (-2)
#define ADC_ESP32C3_ERR_DRIVER          (-3)

/* --------------------------------------------------------------------------
 * 具象实例：字段只描述平台相关状态，且全部使用中性类型
 *
 *   unitHandle    : adc_oneshot_unit_handle_t，NULL 表示单元未打开
 *   caliHandle    : adc_cali_handle_t，NULL 表示校准不可用（readMilliVolts 返回 -2）
 *   unit          : ADC 单元号，1 = ADC1，2 = ADC2
 *   bitWidth      : 输出位宽，9..12（构造时确定，之后不变）
 *   attenuation[] : 各通道已记录的衰减（默认 AdcAtten12dB，与构造时建立的校准曲线一致）
 *   channelReady[]: 各通道是否已成功下发 adc_oneshot_config_channel（0/1）
 *   initialized   : 1 = 单元已打开（与 unitHandle 同步维护）
 * -------------------------------------------------------------------------- */
typedef struct AdcEsp32c3 {
    void*    unitHandle;
    void*    caliHandle;
    uint32_t unit;
    uint8_t  bitWidth;
    uint8_t  attenuation[ADC_ESP32C3_CHANNEL_SLOT_COUNT];
    uint8_t  channelReady[ADC_ESP32C3_CHANNEL_SLOT_COUNT];
    uint8_t  initialized;
} AdcEsp32c3;

/* --------------------------------------------------------------------------
 * 具象构造 / 接口表
 *   NewAdcEsp32c3：自定义参数（单元号 + 位宽），内部调用抽象构造 NewAdc 装配；
 *                   返回后 ADC 单元已打开（adc_oneshot_new_unit 成功），
 *                   校准失败不视为构造失败（caliHandle == NULL）。
 *   AdcEsp32c3Interface：返回注入抽象对象的虚表，字段顺序严格对应 AdcInterface：
 *                   init / deinit / configureChannel / readRaw /
 *                   readMilliVolts / deleteInstance
 * -------------------------------------------------------------------------- */
Adc* NewAdcEsp32c3(uint32_t unit, uint8_t bitWidth);
AdcInterface AdcEsp32c3Interface(void);

#ifdef __cplusplus
}
#endif

#endif /* _ADC_ESP32C3_H */
