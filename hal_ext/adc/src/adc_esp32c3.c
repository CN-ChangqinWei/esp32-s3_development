/**
 * @file adc_esp32c3.c
 * @brief ADC 具象派生类实现：ESP32-C3（ESP-IDF v5.5.x，SAR ADC one-shot）。
 *
 * 平台映射说明（抽象虚表 -> ESP-IDF esp_adc API）：
 *   init             : adc_oneshot_new_unit + adc_cali_create_scheme_curve_fitting
 *                      （幂等：单元已打开直接返回 0）
 *   deinit           : adc_cali_delete_scheme_curve_fitting + adc_oneshot_del_unit
 *                      （各自判空，可重复调用）
 *   configureChannel : adc_oneshot_config_channel（atten 按枚举映射，bitwidth 用本实例位宽）
 *   readRaw          : 懒配置通道 -> adc_oneshot_read
 *   readMilliVolts   : readRaw -> adc_cali_raw_to_voltage（无校准句柄返回 -2）
 *   deleteInstance   : deinit -> adcFree
 *
 * 依赖：esp_adc（adc_oneshot.h / adc_cali.h / adc_cali_scheme.h）、soc（SOC_ADC_RTC_MAX_BITWIDTH）。
 * 本文件是唯一允许出现 ESP-IDF 类型的 ADC 源文件。
 */

#include "adc_esp32c3.h"

#include <string.h>

#include "esp_err.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "soc/soc_caps.h"

/* ESP32-C3 的 SOC_ADC_RTC_MAX_BITWIDTH 为 12；若芯片能力低于本模块上限，
 * 说明常量与平台不匹配，编译期直接报错，避免运行期静默降精度。 */
#if defined(SOC_ADC_RTC_MAX_BITWIDTH) && (SOC_ADC_RTC_MAX_BITWIDTH < 12)
#error "SOC_ADC_RTC_MAX_BITWIDTH < 12，与 ADC_ESP32C3_MAX_BITWIDTH 不一致"
#endif

/* --------------------------------------------------------------------------
 * 返回值常量（与 adc.h / adc_esp32c3.h 的对外约定保持一致）
 * -------------------------------------------------------------------------- */
#define ADC_ESP32C3_OK              (0)
#define ADC_ESP32C3_ERR_PARAM       (-1)

/* 构造时统一建立的校准衰减：与各通道默认衰减一致，保证 mV 换算准确 */
#define ADC_ESP32C3_CALI_ATTEN      ADC_ATTEN_DB_12

/* ==========================================================================
 * 内部工具（全部 static，不向上暴露）
 * ========================================================================== */

/* 单元号 -> adc_unit_t（ESP32-C3 仅 ADC1 / ADC2） */
static int adcEsp32c3MapUnit(uint32_t unit, adc_unit_t* outUnitId)
{
    if (NULL == outUnitId) {
        return ADC_ESP32C3_ERR_PARAM;
    }

    switch (unit) {
    case ADC_ESP32C3_UNIT_1:
        *outUnitId = ADC_UNIT_1;
        return ADC_ESP32C3_OK;
    case ADC_ESP32C3_UNIT_2:
        *outUnitId = ADC_UNIT_2;
        return ADC_ESP32C3_OK;
    default:
        return ADC_ESP32C3_ERR_PARAM;
    }
}

/* 衰减枚举 -> adc_atten_t */
static int adcEsp32c3MapAtten(AdcAttenuation attenuation, adc_atten_t* outAtten)
{
    if (NULL == outAtten) {
        return ADC_ESP32C3_ERR_PARAM;
    }

    switch (attenuation) {
    case AdcAtten0dB:
        *outAtten = ADC_ATTEN_DB_0;
        return ADC_ESP32C3_OK;
    case AdcAtten2_5dB:
        *outAtten = ADC_ATTEN_DB_2_5;
        return ADC_ESP32C3_OK;
    case AdcAtten6dB:
        *outAtten = ADC_ATTEN_DB_6;
        return ADC_ESP32C3_OK;
    case AdcAtten12dB:
        *outAtten = ADC_ATTEN_DB_12;
        return ADC_ESP32C3_OK;
    default:
        return ADC_ESP32C3_ERR_PARAM;
    }
}

/* 位宽（9..12）-> adc_bitwidth_t */
static int adcEsp32c3MapBitwidth(uint8_t bitWidth, adc_bitwidth_t* outBitwidth)
{
    if (NULL == outBitwidth) {
        return ADC_ESP32C3_ERR_PARAM;
    }

    switch (bitWidth) {
    case ADC_ESP32C3_MIN_BITWIDTH:
        *outBitwidth = ADC_BITWIDTH_9;
        return ADC_ESP32C3_OK;
    case 10:
        *outBitwidth = ADC_BITWIDTH_10;
        return ADC_ESP32C3_OK;
    case 11:
        *outBitwidth = ADC_BITWIDTH_11;
        return ADC_ESP32C3_OK;
    case ADC_ESP32C3_MAX_BITWIDTH:
        *outBitwidth = ADC_BITWIDTH_12;
        return ADC_ESP32C3_OK;
    default:
        return ADC_ESP32C3_ERR_PARAM;
    }
}

/* 通道合法性：ADC1 -> 0..4，ADC2 -> 0 */
static int adcEsp32c3CheckChannel(uint32_t unit, uint32_t channel)
{
    uint32_t maxChannel = 0;

    if (ADC_ESP32C3_UNIT_1 == unit) {
        maxChannel = ADC_ESP32C3_ADC1_MAX_CHANNEL;
    } else if (ADC_ESP32C3_UNIT_2 == unit) {
        maxChannel = ADC_ESP32C3_ADC2_MAX_CHANNEL;
    } else {
        return ADC_ESP32C3_ERR_PARAM;
    }

    /* 同时保证可安全索引 attenuation[] / channelReady[] */
    if (channel > maxChannel) {
        return ADC_ESP32C3_ERR_PARAM;
    }
    if (channel >= (uint32_t)ADC_ESP32C3_CHANNEL_SLOT_COUNT) {
        return ADC_ESP32C3_ERR_PARAM;
    }

    return ADC_ESP32C3_OK;
}

/* --------------------------------------------------------------------------
 * 打开 ADC 单元 + 建立曲线拟合校准（幂等：已打开直接返回 0）
 *   单元打开失败 -> -3（无残留句柄）
 *   校准失败     -> 不算失败：caliHandle 保持 NULL，readMilliVolts 返回 -2
 * -------------------------------------------------------------------------- */
static int adcEsp32c3OpenUnit(AdcEsp32c3* self)
{
    adc_oneshot_unit_init_cfg_t     unitCfg;
    adc_oneshot_unit_handle_t       unitHandle = NULL;
    adc_cali_curve_fitting_config_t caliCfg;
    adc_cali_handle_t               caliHandle = NULL;
    adc_unit_t                      unitId     = ADC_UNIT_1;
    adc_bitwidth_t                  bitwidth   = ADC_BITWIDTH_12;

    if (NULL == self) {
        return ADC_ESP32C3_ERR_PARAM;
    }
    if (NULL != self->unitHandle) {
        self->initialized = 1;
        return ADC_ESP32C3_OK; /* 幂等 */
    }
    if (ADC_ESP32C3_OK != adcEsp32c3MapUnit(self->unit, &unitId)) {
        return ADC_ESP32C3_ERR_PARAM;
    }
    if (ADC_ESP32C3_OK != adcEsp32c3MapBitwidth(self->bitWidth, &bitwidth)) {
        return ADC_ESP32C3_ERR_PARAM;
    }

    memset(&unitCfg, 0, sizeof(unitCfg));
    unitCfg.unit_id  = unitId;
    /* ESP32-C3 无 RTC ADC 控制器，oneshot 时钟源枚举等价于 digi 时钟源 */
    unitCfg.clk_src  = ADC_DIGI_CLK_SRC_DEFAULT;
    unitCfg.ulp_mode = ADC_ULP_MODE_DISABLE;

    if (ESP_OK != adc_oneshot_new_unit(&unitCfg, &unitHandle)) {
        return ADC_ESP32C3_ERR_DRIVER;
    }

    /* 曲线拟合校准：按 unit + 12dB + 本实例位宽建立；
     * chan 字段仅供部分芯片的片上校准分支取用，曲线本身按单元生效，
     * 故统一填 CH0，通道差异由衰减/位宽（已固定一致）覆盖。 */
    memset(&caliCfg, 0, sizeof(caliCfg));
    caliCfg.unit_id  = unitId;
    caliCfg.chan     = ADC_CHANNEL_0;
    caliCfg.atten    = ADC_ESP32C3_CALI_ATTEN;
    caliCfg.bitwidth = bitwidth;
    if (ESP_OK != adc_cali_create_scheme_curve_fitting(&caliCfg, &caliHandle)) {
        caliHandle = NULL; /* 校准不可用不阻塞构造 */
    }

    self->unitHandle  = (void*)unitHandle;
    self->caliHandle  = (void*)caliHandle;
    self->initialized = 1;

    return ADC_ESP32C3_OK;
}

/* --------------------------------------------------------------------------
 * 关闭 ADC 单元 + 释放校准（各自判空，可重复调用）
 * -------------------------------------------------------------------------- */
static void adcEsp32c3CloseUnit(AdcEsp32c3* self)
{
    if (NULL == self) {
        return;
    }

    if (NULL != self->caliHandle) {
        (void)adc_cali_delete_scheme_curve_fitting((adc_cali_handle_t)self->caliHandle);
        self->caliHandle = NULL;
    }

    if (NULL != self->unitHandle) {
        (void)adc_oneshot_del_unit((adc_oneshot_unit_handle_t)self->unitHandle);
        self->unitHandle = NULL;
    }

    memset(self->channelReady, 0, sizeof(self->channelReady));
    self->initialized = 0;
}

/* --------------------------------------------------------------------------
 * 真正下发通道配置（不判断 channelReady），成功 0
 * -------------------------------------------------------------------------- */
static int adcEsp32c3ApplyChannel(AdcEsp32c3* self, uint32_t channel,
                                 AdcAttenuation attenuation)
{
    adc_oneshot_chan_cfg_t chanCfg;
    adc_atten_t            atten    = ADC_ATTEN_DB_12;
    adc_bitwidth_t         bitwidth = ADC_BITWIDTH_12;

    if (NULL == self) {
        return ADC_ESP32C3_ERR_PARAM;
    }
    if (NULL == self->unitHandle) {
        return ADC_ESP32C3_ERR_PARAM; /* 单元未打开：请先 init 或重新构造 */
    }
    if (ADC_ESP32C3_OK != adcEsp32c3CheckChannel(self->unit, channel)) {
        return ADC_ESP32C3_ERR_PARAM;
    }
    if (ADC_ESP32C3_OK != adcEsp32c3MapAtten(attenuation, &atten)) {
        return ADC_ESP32C3_ERR_PARAM;
    }
    if (ADC_ESP32C3_OK != adcEsp32c3MapBitwidth(self->bitWidth, &bitwidth)) {
        return ADC_ESP32C3_ERR_PARAM;
    }

    memset(&chanCfg, 0, sizeof(chanCfg));
    chanCfg.atten    = atten;
    chanCfg.bitwidth = bitwidth;

    if (ESP_OK != adc_oneshot_config_channel((adc_oneshot_unit_handle_t)self->unitHandle,
                                            (adc_channel_t)channel, &chanCfg)) {
        self->channelReady[channel] = 0;
        return ADC_ESP32C3_ERR_DRIVER;
    }

    self->attenuation[channel]  = (uint8_t)attenuation;
    self->channelReady[channel] = 1;

    return ADC_ESP32C3_OK;
}

/* --------------------------------------------------------------------------
 * 懒配置：adc_oneshot_read 要求通道已配置，未配置过则按已记录的衰减补下发
 * -------------------------------------------------------------------------- */
static int adcEsp32c3EnsureChannel(AdcEsp32c3* self, uint32_t channel)
{
    if (NULL == self) {
        return ADC_ESP32C3_ERR_PARAM;
    }
    if (NULL == self->unitHandle) {
        return ADC_ESP32C3_ERR_PARAM;
    }
    if (ADC_ESP32C3_OK != adcEsp32c3CheckChannel(self->unit, channel)) {
        return ADC_ESP32C3_ERR_PARAM;
    }
    if (0U != self->channelReady[channel]) {
        return ADC_ESP32C3_OK;
    }

    return adcEsp32c3ApplyChannel(self, channel,
                                  (AdcAttenuation)self->attenuation[channel]);
}

/* ==========================================================================
 * 虚表实现：首参恒为 void* instance，首行取回具象实例
 * ========================================================================== */

/* 初始化（幂等）：已打开则直接 0；被 deinit 过则重新打开单元并重建校准 */
static int adcEsp32c3Init(void* instance)
{
    AdcEsp32c3* self = (AdcEsp32c3*)instance;

    if (NULL == self) {
        return ADC_ESP32C3_ERR_PARAM;
    }
    if (NULL != self->unitHandle) {
        self->initialized = 1;
        return ADC_ESP32C3_OK;
    }

    return adcEsp32c3OpenUnit(self);
}

/* 反初始化（幂等）：释放校准 + 删除单元，句柄置空防悬空 */
static int adcEsp32c3Deinit(void* instance)
{
    AdcEsp32c3* self = (AdcEsp32c3*)instance;

    if (NULL == self) {
        return ADC_ESP32C3_ERR_PARAM;
    }

    adcEsp32c3CloseUnit(self);

    return ADC_ESP32C3_OK;
}

/* 配置通道：记录衰减并立即下发；重复调用可运行时改衰减 */
static int adcEsp32c3ConfigureChannel(void* instance, uint32_t channel,
                                     AdcAttenuation attenuation)
{
    AdcEsp32c3* self = (AdcEsp32c3*)instance;

    if (NULL == self) {
        return ADC_ESP32C3_ERR_PARAM;
    }
    if (ADC_ESP32C3_OK != adcEsp32c3CheckChannel(self->unit, channel)) {
        return ADC_ESP32C3_ERR_PARAM;
    }

    return adcEsp32c3ApplyChannel(self, channel, attenuation);
}

/* 读原始值：通道未配置时按已记录衰减懒配置后再读 */
static int adcEsp32c3ReadRaw(void* instance, uint32_t channel, uint32_t* rawValue)
{
    AdcEsp32c3* self = (AdcEsp32c3*)instance;
    int         rawInt = 0;
    int         rc     = ADC_ESP32C3_OK;

    if (NULL == self || NULL == rawValue) {
        return ADC_ESP32C3_ERR_PARAM;
    }
    if (NULL == self->unitHandle) {
        return ADC_ESP32C3_ERR_PARAM;
    }

    rc = adcEsp32c3EnsureChannel(self, channel);
    if (ADC_ESP32C3_OK != rc) {
        return rc;
    }

    if (ESP_OK != adc_oneshot_read((adc_oneshot_unit_handle_t)self->unitHandle,
                                   (adc_channel_t)channel, &rawInt)) {
        return ADC_ESP32C3_ERR_DRIVER;
    }

    *rawValue = (uint32_t)rawInt;

    return ADC_ESP32C3_OK;
}

/* 读毫伏：无校准句柄返回 -2；否则 raw -> voltage 由底层曲线拟合换算 */
static int adcEsp32c3ReadMilliVolts(void* instance, uint32_t channel,
                                    uint32_t* milliVolts)
{
    AdcEsp32c3* self = (AdcEsp32c3*)instance;
    uint32_t    raw  = 0;
    int         mvInt = 0;
    int         rc    = ADC_ESP32C3_OK;

    if (NULL == self || NULL == milliVolts) {
        return ADC_ESP32C3_ERR_PARAM;
    }
    if (NULL == self->unitHandle) {
        return ADC_ESP32C3_ERR_PARAM;
    }
    if (NULL == self->caliHandle) {
        return ADC_ESP32C3_ERR_NO_CALIBRATION; /* -2：校准不可用 */
    }

    rc = adcEsp32c3ReadRaw(instance, channel, &raw);
    if (ADC_ESP32C3_OK != rc) {
        return rc;
    }

    if (ESP_OK != adc_cali_raw_to_voltage((adc_cali_handle_t)self->caliHandle,
                                          (int)raw, &mvInt)) {
        return ADC_ESP32C3_ERR_DRIVER;
    }
    if (mvInt < 0) {
        return ADC_ESP32C3_ERR_DRIVER;
    }

    *milliVolts = (uint32_t)mvInt;

    return ADC_ESP32C3_OK;
}

/* 具象析构：先 deinit（释放校准 + 删除单元），再释放具象实例 */
static void adcEsp32c3DeleteInstance(void* instance)
{
    AdcEsp32c3* self = (AdcEsp32c3*)instance;

    if (NULL == self) {
        return;
    }

    adcEsp32c3CloseUnit(self);

    if (NULL != adcFree) {
        adcFree(self);
    }
    self = NULL;
}

/* ==========================================================================
 * 接口表 + 具象构造
 * ========================================================================== */

AdcInterface AdcEsp32c3Interface(void)
{
    _RETURN_ADC_INTERFACE_WITH(adcEsp32c3Init,
                               adcEsp32c3Deinit,
                               adcEsp32c3ConfigureChannel,
                               adcEsp32c3ReadRaw,
                               adcEsp32c3ReadMilliVolts,
                               adcEsp32c3DeleteInstance);
}

Adc* NewAdcEsp32c3(uint32_t unit, uint8_t bitWidth)
{
    AdcEsp32c3* self               = NULL;
    Adc*        result             = NULL;
    uint8_t     effectiveBitWidth  = bitWidth;
    uint32_t    index              = 0;

    /* 参数校验：unit 仅 1 / 2；bitWidth 为 0 时按 12 处理，其余须在 9..12 */
    if ((ADC_ESP32C3_UNIT_1 != unit) && (ADC_ESP32C3_UNIT_2 != unit)) {
        return NULL;
    }
    if (0U == effectiveBitWidth) {
        effectiveBitWidth = ADC_ESP32C3_DEFAULT_BITWIDTH;
    }
    if (((uint32_t)effectiveBitWidth < ADC_ESP32C3_MIN_BITWIDTH) ||
        ((uint32_t)effectiveBitWidth > ADC_ESP32C3_MAX_BITWIDTH)) {
        return NULL;
    }
    if (NULL == adcMalloc) {
        return NULL;
    }

    self = (AdcEsp32c3*)adcMalloc(sizeof(AdcEsp32c3));
    if (NULL == self) {
        return NULL;
    }

    memset(self, 0, sizeof(AdcEsp32c3));
    self->unit     = unit;
    self->bitWidth = effectiveBitWidth;

    /* 各通道默认衰减与构造时建立的校准曲线（12dB）保持一致 */
    for (index = 0; index < (uint32_t)ADC_ESP32C3_CHANNEL_SLOT_COUNT; ++index) {
        self->attenuation[index]  = (uint8_t)AdcAtten12dB;
        self->channelReady[index] = 0;
    }

    /* 平台初始化：打开 ADC 单元（校准失败不视为失败） */
    if (ADC_ESP32C3_OK != adcEsp32c3OpenUnit(self)) {
        if (NULL != adcFree) {
            adcFree(self);
        }
        self = NULL;
        return NULL;
    }

    /* 抽象构造装配（禁止手工赋值 instance / interfaces） */
    result = NewAdc((void*)self, AdcEsp32c3Interface());
    if (NULL == result) {
        adcEsp32c3DeleteInstance((void*)self); /* 回收 unit / cali 后释放具象实例 */
        self = NULL;
        return NULL;
    }

    return result;
}
