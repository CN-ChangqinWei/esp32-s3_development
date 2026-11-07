/**
 * @file adc.c
 * @brief ADC 抽象父类实现：内存钩子默认值 + 抽象构造/析构 + 包装函数透传。
 *
 * 本文件不含任何平台相关代码（无 ESP-IDF / FreeRTOS 依赖），
 * 只做「分配 → 存 instance → 存 interfaces」以及「判空 → 透传」。
 */

#include "adc.h"

#include <stdlib.h>

/* ------------------------------------------------------------------
 * 返回值约定（与头文件注释保持一致）
 * ------------------------------------------------------------------ */
#define ADC_RET_OK              (0)
#define ADC_RET_ERR_PARAM       (-1)
#define ADC_RET_ERR_UNSUPPORTED (-2)

/* ------------------------------------------------------------------
 * 内存钩子默认实现：外部可在创建对象之前替换为平台专用分配器
 *     adcMalloc = pvPortMalloc;   adcFree = vPortFree;
 * ------------------------------------------------------------------ */
void* (*adcMalloc)(size_t size) = malloc;
void  (*adcFree)(void* p)       = free;

/* ------------------------------------------------------------------
 * 内部工具：句柄 / 枚举入参校验
 * ------------------------------------------------------------------ */

/* 抽象对象与具体实例都必须存在；0 表示可用，-1 表示参数非法 */
static int adcCheckHandle(const Adc* self)
{
    if (NULL == self) {
        return ADC_RET_ERR_PARAM;
    }
    if (NULL == self->instance) {
        return ADC_RET_ERR_PARAM;
    }
    return ADC_RET_OK;
}

/* 衰减枚举越界一律视为参数非法 */
static int adcCheckAttenuation(AdcAttenuation attenuation)
{
    if (((int)attenuation < (int)AdcAtten0dB) ||
        ((int)attenuation > (int)AdcAtten12dB)) {
        return ADC_RET_ERR_PARAM;
    }
    return ADC_RET_OK;
}

/* ------------------------------------------------------------------
 * 抽象构造：只做三件事 —— 分配抽象对象 / 赋 instance / 赋 interfaces
 * 不 malloc 具体类型、不解析任何业务参数。
 * instance 为空或未提供 deleteInstance 时直接失败，避免泄漏具象实例。
 * ------------------------------------------------------------------ */
Adc* NewAdc(void* instance, AdcInterface interfaces)
{
    Adc* self = NULL;

    if (NULL == instance) {
        return NULL;
    }
    if (NULL == interfaces.deleteInstance) {
        return NULL;
    }
    if (NULL == adcMalloc) {
        return NULL;
    }

    self = (Adc*)adcMalloc(sizeof(Adc));
    if (NULL == self) {
        return NULL;
    }

    self->instance   = instance;
    self->interfaces = interfaces;

    return self;
}

/* ------------------------------------------------------------------
 * 抽象析构：先销毁具体实例，再释放抽象对象，最后置空入参指针
 * ------------------------------------------------------------------ */
void DeleteAdc(Adc** pAdc)
{
    Adc* self = NULL;

    if (NULL == pAdc) {
        return;
    }

    self = *pAdc;
    if (NULL == self) {
        return;
    }

    if (NULL != self->interfaces.deleteInstance) {
        self->interfaces.deleteInstance(self->instance);
    }

    self->instance                  = NULL;
    self->interfaces.deleteInstance = NULL;

    if (NULL != adcFree) {
        adcFree(self);
    }
    self = NULL;

    *pAdc = NULL;
}

/* ------------------------------------------------------------------
 * 默认配置：纯值类型，不涉及任何全局状态
 *   attenuation = AdcAtten12dB（0~3.3V 量程）、bitWidth = 12
 * ------------------------------------------------------------------ */
AdcConfig AdcDefaultConfig(uint32_t unit)
{
    AdcConfig config;

    config.unit        = unit;
    config.attenuation = AdcAtten12dB;
    config.bitWidth    = 12;

    return config;
}

/* ------------------------------------------------------------------
 * 包装函数：先判空 / 判非法入参，再透传虚表
 *   返回 0  ：虚表调用成功返回
 *   返回 -1 ：adc 为空、instance 为空、输出指针为空、attenuation 越界
 *   返回 -2 ：对应虚表槽位为 NULL（具象层未实现）
 * ------------------------------------------------------------------ */
int AdcInit(Adc* adc)
{
    int rc = adcCheckHandle(adc);

    if (ADC_RET_OK != rc) {
        return rc;
    }
    if (NULL == adc->interfaces.init) {
        return ADC_RET_ERR_UNSUPPORTED;
    }

    return adc->interfaces.init(adc->instance);
}

int AdcDeinit(Adc* adc)
{
    int rc = adcCheckHandle(adc);

    if (ADC_RET_OK != rc) {
        return rc;
    }
    if (NULL == adc->interfaces.deinit) {
        return ADC_RET_ERR_UNSUPPORTED;
    }

    return adc->interfaces.deinit(adc->instance);
}

int AdcConfigureChannel(Adc* adc, uint32_t channel, AdcAttenuation attenuation)
{
    int rc = adcCheckHandle(adc);

    if (ADC_RET_OK != rc) {
        return rc;
    }
    rc = adcCheckAttenuation(attenuation);
    if (ADC_RET_OK != rc) {
        return rc;
    }
    if (NULL == adc->interfaces.configureChannel) {
        return ADC_RET_ERR_UNSUPPORTED;
    }

    return adc->interfaces.configureChannel(adc->instance, channel, attenuation);
}

int AdcReadRaw(Adc* adc, uint32_t channel, uint32_t* rawValue)
{
    int rc = adcCheckHandle(adc);

    if (ADC_RET_OK != rc || NULL == rawValue) {
        return ADC_RET_ERR_PARAM;
    }
    if (NULL == adc->interfaces.readRaw) {
        return ADC_RET_ERR_UNSUPPORTED;
    }

    return adc->interfaces.readRaw(adc->instance, channel, rawValue);
}

int AdcReadMilliVolts(Adc* adc, uint32_t channel, uint32_t* milliVolts)
{
    int rc = adcCheckHandle(adc);

    if (ADC_RET_OK != rc || NULL == milliVolts) {
        return ADC_RET_ERR_PARAM;
    }
    if (NULL == adc->interfaces.readMilliVolts) {
        return ADC_RET_ERR_UNSUPPORTED;
    }

    return adc->interfaces.readMilliVolts(adc->instance, channel, milliVolts);
}
