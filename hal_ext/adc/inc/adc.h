#ifndef _ADC_H
#define _ADC_H

/* ==================================================================
 * adc.h —— ADC 抽象父类（OOP in C）
 *
 * 结构：
 *   抽象对象 Adc = { void* instance; AdcInterface interfaces; }
 *   具体实现（如 hal/adc/src/adc_esp32c3.c）负责 instance 的真实类型
 *   与全部虚表函数的实现；抽象层只做「装配 + 判空 + 透传」。
 *
 * 依赖：
 *   本文件只允许 <stdint.h> / <stddef.h>，
 *   禁止包含 freertos、driver 下的平台头文件，以及 esp_*.h 等平台头文件。
 *
 * 字段归属（重要）：
 *   抽象层只持有 instance 与虚表；通道号 / 引脚号 / ADC 单元句柄等
 *   平台相关数据一律放在具象派生类的私有结构体里。
 *   AdcConfig 为纯值类型，可出现在抽象头中，只描述「单元号 + 默认
 *   衰减 + 输出位宽」，不含任何通道号 / 引脚号。
 *
 * 返回值约定：
 *    0   成功
 *   -1   参数非法（adc 为空、instance 为空、输出指针为空、attenuation
 *        越界；bitWidth == 0 由具象层在消费 AdcConfig 时按同一约定返回 -1）
 *   -2   未实现 / 不支持（对应虚表槽位为 NULL）
 *   其余负值为具象层自定义错误码
 * ================================================================== */

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------
 * 内存钩子：默认指向 malloc / free（定义在 adc.c），
 * 外部可在创建对象之前替换为平台专用分配器：
 *     adcMalloc = pvPortMalloc;  adcFree = vPortFree;
 * ------------------------------------------------------------------ */
extern void* (*adcMalloc)(size_t size);
extern void  (*adcFree)(void* p);

/* ------------------------------------------------------------------
 * 枚举 / 纯值类型配置
 * 命名统一用 CamelCase，避免与 ESP-IDF 的 ADC_ATTEN_DB_12、
 * ADC_UNIT_1 等全大写标识符冲突（具象 .c 会同时包含 esp_adc 下的头文件）。
 * ------------------------------------------------------------------ */
typedef enum AdcAttenuation {
    AdcAtten0dB = 0,      /* 0 dB    */
    AdcAtten2_5dB,        /* 2.5 dB  */
    AdcAtten6dB,          /* 6 dB    */
    AdcAtten12dB          /* 12 dB   */
} AdcAttenuation;

typedef struct AdcConfig {
    uint32_t       unit;          /* ADC 单元号，ESP32-C3 仅 ADC_UNIT_1 / ADC_UNIT_2 */
    AdcAttenuation attenuation;   /* 默认衰减 */
    uint8_t        bitWidth;      /* 输出位宽，通常 12，禁止为 0 */
} AdcConfig;

/* ------------------------------------------------------------------
 * 虚表：由具体实现填充；抽象层只调用，不实现
 * 首参恒为 void* instance（具象实例），具象侧实现为 static 函数。
 * 析构槽位固定命名 deleteInstance（不得使用 C++ 保留字）。
 * ------------------------------------------------------------------ */
typedef struct AdcInterface {
    int  (*init)(void* instance);
    int  (*deinit)(void* instance);
    int  (*configureChannel)(void* instance, uint32_t channel, AdcAttenuation attenuation);
    int  (*readRaw)(void* instance, uint32_t channel, uint32_t* rawValue);
    int  (*readMilliVolts)(void* instance, uint32_t channel, uint32_t* milliVolts);
    void (*deleteInstance)(void* instance);
} AdcInterface;

/* ------------------------------------------------------------------
 * 抽象对象：只允许 instance + interfaces 两个字段
 * ------------------------------------------------------------------ */
typedef struct Adc {
    void*        instance;
    AdcInterface interfaces;
} Adc;

/* ==================================================================
 * _RETURN_ADC_INTERFACE_WITH
 * 供具体实现集中返回接口表：展开为「建表 + return interfaces」，
 * 以 return 语句结尾、不带分号，调用处写 `;` 即可。
 *
 * 用法（具象 .c）：
 *   AdcInterface AdcEsp32c3Interface(void)
 *   {
 *       _RETURN_ADC_INTERFACE_WITH(esp32c3Init, esp32c3Deinit,
 *                                  esp32c3ConfigureChannel,
 *                                  esp32c3ReadRaw, esp32c3ReadMilliVolts,
 *                                  esp32c3DeleteInstance);
 *   }
 *
 * _RETURN_ADC_INTERFACE 等价于传入约定名：
 *   init / deinit / configureChannel / readRaw / readMilliVolts / deleteInstance
 * ================================================================== */
#define _RETURN_ADC_INTERFACE_WITH(initFn, deinitFn, configureChannelFn, readRawFn, \
                                   readMilliVoltsFn, deleteFn)                      \
    AdcInterface interfaces = {                                                      \
        .init             = (initFn),                                                \
        .deinit           = (deinitFn),                                              \
        .configureChannel = (configureChannelFn),                                    \
        .readRaw          = (readRawFn),                                             \
        .readMilliVolts   = (readMilliVoltsFn),                                      \
        .deleteInstance   = (deleteFn)                                               \
    };                                                                               \
    return interfaces

#define _RETURN_ADC_INTERFACE                                                     \
    _RETURN_ADC_INTERFACE_WITH(init, deinit, configureChannel, readRaw,            \
                               readMilliVolts, deleteInstance)

/* ------------------------------------------------------------------
 * 抽象构造 / 抽象析构
 *   NewAdc   ：只做「分配 + 赋 instance + 赋 interfaces」，
 *              instance 或 interfaces.deleteInstance 为 NULL 时返回 NULL。
 *   DeleteAdc：先 deleteInstance，再 adcFree 抽象对象，最后把入参置 NULL。
 * ------------------------------------------------------------------ */
Adc* NewAdc(void* instance, AdcInterface interfaces);
void DeleteAdc(Adc** pAdc);

/* ------------------------------------------------------------------
 * 默认配置：attenuation = AdcAtten12dB、bitWidth = 12，unit 由调用者给出
 * ------------------------------------------------------------------ */
AdcConfig AdcDefaultConfig(uint32_t unit);

/* ------------------------------------------------------------------
 * 包装函数：先判空 / 判非法入参，再透传虚表
 * ------------------------------------------------------------------ */
int AdcInit(Adc* adc);
int AdcDeinit(Adc* adc);
int AdcConfigureChannel(Adc* adc, uint32_t channel, AdcAttenuation attenuation);
int AdcReadRaw(Adc* adc, uint32_t channel, uint32_t* rawValue);
int AdcReadMilliVolts(Adc* adc, uint32_t channel, uint32_t* milliVolts);

#ifdef __cplusplus
}
#endif

#endif /* _ADC_H */
