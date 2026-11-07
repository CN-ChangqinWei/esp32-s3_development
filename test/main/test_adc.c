/**
 * @file test_adc.c
 * @brief hal/adc 抽象父类单元测试：用 FakeAdc 具象验证
 *        「抽象构造 / 抽象析构 / 判空护栏 / 虚表透传 / 默认配置 / 内存钩子配平」。
 *
 * 测试边界：
 *   - 只测 hal/adc/inc/adc.h 暴露的抽象层（装配 + 判空 + 透传），
 *     不触碰任何真实 ADC 单元、通道、引脚，也不包含任何 ESP-IDF 头；
 *   - 具体实现由本文件内的 FakeAdc + fakeInterface() 提供，
 *     不使用 _RETURN_ADC_INTERFACE_WITH / _RETURN_ADC_INTERFACE 宏，
 *     避免依赖具象侧（adc_esp32c3.c）的具体函数名；
 *   - 只依赖 unity 与 <stdint.h> / <stdlib.h>。
 *
 * 入口：void RunAdcTests(void);（由 test/main/test_main.c 调用）
 *
 * 用例清单：
 *   1. NewAdc(NULL, ...)                    → NULL
 *   2. NewAdc(..., deleteInstance == NULL)  → NULL
 *   3. 正常装配：instance 原样保存、接口表逐项一致
 *   4. DeleteAdc 恰好调用 deleteInstance 1 次并置空句柄
 *   5. DeleteAdc(NULL) / DeleteAdc(&空指针) 安全
 *   6. AdcDefaultConfig(1)：unit / attenuation / bitWidth 符合约定
 *   7. AdcInit / AdcDeinit 正常透传且各调用 1 次
 *   8. 空句柄：AdcInit / AdcDeinit / AdcConfigureChannel / AdcReadRaw /
 *      AdcReadMilliVolts 一律 -1，虚表零调用
 *   9. 非法衰减值（越界枚举）→ -1，虚表零调用
 *  10. 输出指针为 NULL：AdcReadRaw(adc, 0, NULL) / AdcReadMilliVolts(adc, 0, NULL)
 *      → -1，虚表零调用
 *  11. AdcConfigureChannel 的 channel / attenuation 原样透传
 *  12. AdcReadRaw 把 fake 的原始值带出，channel 原样透传
 *  13. AdcReadMilliVolts 把毫伏值带出，且 -2（校准不可用）原样上报
 *  14. adcMalloc / adcFree 计数配平，恢复原钩子后再断言
 *
 * 说明：被测代码语义以 adc.h 的返回值约定为准，断言统一用
 *       ADC_TEST_OK(0) / ADC_TEST_ERR_PARAM(-1) / ADC_TEST_ERR_UNSUPPORTED(-2)。
 */

#include <stdint.h>
#include <stdlib.h>

#include "unity.h"

#include "adc.h"

/* hal/adc/inc/adc.h 未导出错误码宏，此处仅为本文件内部使用的复刻常量 */
#define ADC_TEST_OK              (0)
#define ADC_TEST_ERR_PARAM       (-1)
#define ADC_TEST_ERR_UNSUPPORTED (-2)

/* 本文件唯一导出符号：由 test/main/test_main.c 声明并调用 */
void RunAdcTests(void);

/* ==========================================================================
 * 1. 测试用具体实现（fake）
 *    只做入参快照与计数，绝不访问硬件；所有虚表函数首参为 instance，
 *    体内取 FakeAdc* self 后写 self->field。
 * ========================================================================== */

typedef struct FakeAdc {
    /* 各虚表函数被调用的次数（用于「恰好 1 次」与「零调用」断言） */
    int initCalls;
    int deinitCalls;
    int configureChannelCalls;
    int readRawCalls;
    int readMilliVoltsCalls;
    int deleteCalls;

    /* 最近一次调用的入参快照（用于「原样透传」断言） */
    uint32_t       configuredChannel;
    uint32_t       readRawChannel;
    uint32_t       readMilliVoltsChannel;
    AdcAttenuation lastAttenuation;

    /* readRaw / readMilliVolts 要带出给抽象层的值与其返回码 */
    int      readRawResult;
    uint32_t readRawValue;
    int      readMilliVoltsResult;
    uint32_t readMilliVoltsValue;
} FakeAdc;

/* 唯一 fake 实例：抽象对象通过 instance 指针持有它 */
static FakeAdc fakeAdc;

/* --------------------------------------------------------------------------
 * fake 状态复位：每个用例首尾各调用一次，保证用例之间互不影响
 * -------------------------------------------------------------------------- */
static void fakeReset(FakeAdc* self)
{
    if (self == NULL) {
        return;
    }

    self->initCalls             = 0;
    self->deinitCalls           = 0;
    self->configureChannelCalls = 0;
    self->readRawCalls          = 0;
    self->readMilliVoltsCalls   = 0;
    self->deleteCalls           = 0;

    self->configuredChannel    = 0u;
    self->readRawChannel       = 0u;
    self->readMilliVoltsChannel = 0u;
    self->lastAttenuation      = AdcAtten0dB;

    self->readRawResult        = ADC_TEST_OK;
    self->readRawValue         = 0u;
    self->readMilliVoltsResult = ADC_TEST_OK;
    self->readMilliVoltsValue  = 0u;
}

/* 全部虚表调用次数之和（含 deleteInstance），用于「零调用 / 恰好 1 次」断言 */
static int fakeTotalCalls(const FakeAdc* self)
{
    if (self == NULL) {
        return -1;
    }

    return self->initCalls
         + self->deinitCalls
         + self->configureChannelCalls
         + self->readRawCalls
         + self->readMilliVoltsCalls
         + self->deleteCalls;
}

/* --------------------------------------------------------------------------
 * fake 虚表实现
 * -------------------------------------------------------------------------- */

static int fakeInit(void* instance)
{
    FakeAdc* self = (FakeAdc*)instance;

    if (self == NULL) {
        return ADC_TEST_ERR_PARAM;
    }

    self->initCalls++;

    return ADC_TEST_OK;
}

static int fakeDeinit(void* instance)
{
    FakeAdc* self = (FakeAdc*)instance;

    if (self == NULL) {
        return ADC_TEST_ERR_PARAM;
    }

    self->deinitCalls++;

    return ADC_TEST_OK;
}

static int fakeConfigureChannel(void* instance, uint32_t channel, AdcAttenuation attenuation)
{
    FakeAdc* self = (FakeAdc*)instance;

    if (self == NULL) {
        return ADC_TEST_ERR_PARAM;
    }

    self->configureChannelCalls++;
    self->configuredChannel = channel;
    self->lastAttenuation   = attenuation;

    return ADC_TEST_OK;
}

static int fakeReadRaw(void* instance, uint32_t channel, uint32_t* rawValue)
{
    FakeAdc* self = (FakeAdc*)instance;

    if (self == NULL || rawValue == NULL) {
        return ADC_TEST_ERR_PARAM;
    }

    self->readRawCalls++;
    self->readRawChannel = channel;
    *rawValue            = self->readRawValue;

    return self->readRawResult;
}

static int fakeReadMilliVolts(void* instance, uint32_t channel, uint32_t* milliVolts)
{
    FakeAdc* self = (FakeAdc*)instance;

    if (self == NULL || milliVolts == NULL) {
        return ADC_TEST_ERR_PARAM;
    }

    self->readMilliVoltsCalls++;
    self->readMilliVoltsChannel = channel;
    *milliVolts                 = self->readMilliVoltsValue;

    /* 返回码可配置：用例 13 用它模拟「校准不可用」的 -2 */
    return self->readMilliVoltsResult;
}

static void fakeDeleteInstance(void* instance)
{
    FakeAdc* self = (FakeAdc*)instance;

    if (self == NULL) {
        return;
    }

    self->deleteCalls++;
}

/* --------------------------------------------------------------------------
 * 接口表：聚合初始化（不使用 _RETURN_ADC_INTERFACE_WITH 宏）
 * -------------------------------------------------------------------------- */
static AdcInterface fakeInterface(void)
{
    AdcInterface interfaces = {
        .init             = fakeInit,
        .deinit           = fakeDeinit,
        .configureChannel = fakeConfigureChannel,
        .readRaw          = fakeReadRaw,
        .readMilliVolts   = fakeReadMilliVolts,
        .deleteInstance   = fakeDeleteInstance,
    };

    return interfaces;
}

/* 变体：析构项缺失，用于验证抽象构造的入参校验 */
static AdcInterface fakeInterfaceWithoutDeleter(void)
{
    AdcInterface interfaces = fakeInterface();

    interfaces.deleteInstance = NULL;

    return interfaces;
}

/* 变体：全部业务槽位缺失，用于验证包装函数的 -2 分支 */
static AdcInterface fakeInterfaceWithoutBusiness(void)
{
    AdcInterface interfaces = fakeInterface();

    interfaces.init             = NULL;
    interfaces.deinit           = NULL;
    interfaces.configureChannel = NULL;
    interfaces.readRaw          = NULL;
    interfaces.readMilliVolts   = NULL;

    return interfaces;
}

/* 两张接口表是否逐项一致（函数指针同类型比较，可直接 ==） */
static int interfacesEqual(AdcInterface left, AdcInterface right)
{
    if (left.init != right.init) {
        return 0;
    }
    if (left.deinit != right.deinit) {
        return 0;
    }
    if (left.configureChannel != right.configureChannel) {
        return 0;
    }
    if (left.readRaw != right.readRaw) {
        return 0;
    }
    if (left.readMilliVolts != right.readMilliVolts) {
        return 0;
    }
    if (left.deleteInstance != right.deleteInstance) {
        return 0;
    }

    return 1;
}

/* --------------------------------------------------------------------------
 * 断言辅助：1 表示 NewAdc 按契约拒绝了非法入参（返回 NULL）；
 *            0 表示未拒绝 —— 顺手回收对象，避免用例失败时泄漏内存。
 * -------------------------------------------------------------------------- */
static int newAdcRejected(Adc* adc)
{
    if (adc == NULL) {
        return 1;
    }

    DeleteAdc(&adc);

    return 0;
}

/* ==========================================================================
 * 2. 分配器计数版：内部仍走 malloc / free，仅统计次数
 * ========================================================================== */

static int allocCount;
static int freeCount;

static void* countingMalloc(size_t size)
{
    allocCount++;

    return malloc(size);
}

static void countingFree(void* p)
{
    freeCount++;

    free(p);
}

/* ==========================================================================
 * 3. 用例
 * ========================================================================== */

/* 用例 1：instance 为 NULL 时必须拒绝装配 */
static void test_newAdcRejectsNullInstance(void)
{
    fakeReset(&fakeAdc);

    TEST_ASSERT_TRUE_MESSAGE(newAdcRejected(NewAdc(NULL, fakeInterface())),
                             "NewAdc 必须拒绝 NULL instance 并返回 NULL");
    TEST_ASSERT_EQUAL_INT(0, fakeTotalCalls(&fakeAdc));

    fakeReset(&fakeAdc);
}

/* 用例 2：析构项缺失时必须拒绝装配（否则 DeleteAdc 无法销毁具体实例） */
static void test_newAdcRejectsMissingDeleter(void)
{
    fakeReset(&fakeAdc);

    TEST_ASSERT_TRUE_MESSAGE(newAdcRejected(NewAdc(&fakeAdc, fakeInterfaceWithoutDeleter())),
                             "NewAdc 必须拒绝 deleteInstance 为 NULL 的接口表");
    TEST_ASSERT_EQUAL_INT(0, fakeTotalCalls(&fakeAdc));

    fakeReset(&fakeAdc);
}

/* 用例 3：正常装配 —— instance 原样保存、接口表逐项一致、不触碰虚表 */
static void test_newAdcAssemblesInstanceAndInterfaces(void)
{
    Adc*         adc = NULL;
    AdcInterface expected;

    fakeReset(&fakeAdc);
    expected = fakeInterface();

    adc = NewAdc(&fakeAdc, expected);
    TEST_ASSERT_NOT_NULL(adc);

    TEST_ASSERT_EQUAL_PTR(&fakeAdc, adc->instance);
    TEST_ASSERT_TRUE_MESSAGE(interfacesEqual(expected, adc->interfaces),
                             "抽象对象必须原样保存接口表");
    TEST_ASSERT_EQUAL_INT(0, fakeTotalCalls(&fakeAdc));

    DeleteAdc(&adc);
    TEST_ASSERT_NULL(adc);

    fakeReset(&fakeAdc);
}

/* 用例 4：析构恰好调用 deleteInstance 1 次，并把句柄置空 */
static void test_deleteAdcCallsDeleterOnce(void)
{
    Adc* adc = NULL;

    fakeReset(&fakeAdc);

    adc = NewAdc(&fakeAdc, fakeInterface());
    TEST_ASSERT_NOT_NULL(adc);

    DeleteAdc(&adc);

    TEST_ASSERT_EQUAL_INT(1, fakeAdc.deleteCalls);
    TEST_ASSERT_EQUAL_INT(1, fakeTotalCalls(&fakeAdc));
    TEST_ASSERT_NULL(adc);

    fakeReset(&fakeAdc);
}

/* 用例 5：DeleteAdc 的空入参安全 */
static void test_deleteAdcIsNullSafe(void)
{
    Adc* adc = NULL;

    fakeReset(&fakeAdc);

    DeleteAdc(NULL);   /* 二级指针为 NULL：不崩溃即可 */
    DeleteAdc(&adc);   /* *pAdc 为 NULL：不崩溃，且仍为 NULL */

    TEST_ASSERT_NULL(adc);
    TEST_ASSERT_EQUAL_INT(0, fakeTotalCalls(&fakeAdc));

    fakeReset(&fakeAdc);
}

/* 用例 6：AdcDefaultConfig 的默认值约定 —— 12 dB + 12 bit，unit 由调用者给出 */
static void test_defaultConfigFields(void)
{
    AdcConfig config;

    fakeReset(&fakeAdc);

    config = AdcDefaultConfig(1u);

    TEST_ASSERT_EQUAL_UINT32(1u, config.unit);
    TEST_ASSERT_EQUAL_INT(AdcAtten12dB, config.attenuation);
    TEST_ASSERT_EQUAL_UINT8(12u, config.bitWidth);

    /* 纯值类型：不同 unit 只影响 unit 字段 */
    config = AdcDefaultConfig(2u);
    TEST_ASSERT_EQUAL_UINT32(2u, config.unit);
    TEST_ASSERT_EQUAL_INT(AdcAtten12dB, config.attenuation);
    TEST_ASSERT_EQUAL_UINT8(12u, config.bitWidth);

    TEST_ASSERT_EQUAL_INT(0, fakeTotalCalls(&fakeAdc));

    fakeReset(&fakeAdc);
}

/* 用例 7：AdcInit / AdcDeinit 正常透传，各调用 1 次 */
static void test_initAndDeinitPassThrough(void)
{
    Adc* adc = NULL;

    fakeReset(&fakeAdc);

    adc = NewAdc(&fakeAdc, fakeInterface());
    TEST_ASSERT_NOT_NULL(adc);

    TEST_ASSERT_EQUAL_INT(ADC_TEST_OK, AdcInit(adc));
    TEST_ASSERT_EQUAL_INT(1, fakeAdc.initCalls);

    TEST_ASSERT_EQUAL_INT(ADC_TEST_OK, AdcDeinit(adc));
    TEST_ASSERT_EQUAL_INT(1, fakeAdc.deinitCalls);

    TEST_ASSERT_EQUAL_INT(2, fakeTotalCalls(&fakeAdc));

    DeleteAdc(&adc);
    TEST_ASSERT_NULL(adc);

    fakeReset(&fakeAdc);
}

/* 用例 8：全部包装函数传 NULL 句柄一律返回 -1，虚表零调用 */
static void test_nullHandleIsRejected(void)
{
    uint32_t value = 0u;

    fakeReset(&fakeAdc);

    TEST_ASSERT_EQUAL_INT(ADC_TEST_ERR_PARAM, AdcInit(NULL));
    TEST_ASSERT_EQUAL_INT(ADC_TEST_ERR_PARAM, AdcDeinit(NULL));
    TEST_ASSERT_EQUAL_INT(ADC_TEST_ERR_PARAM, AdcConfigureChannel(NULL, 0u, AdcAtten12dB));
    TEST_ASSERT_EQUAL_INT(ADC_TEST_ERR_PARAM, AdcReadRaw(NULL, 0u, &value));
    TEST_ASSERT_EQUAL_INT(ADC_TEST_ERR_PARAM, AdcReadMilliVolts(NULL, 0u, &value));

    TEST_ASSERT_EQUAL_UINT32(0u, value);   /* 输出参数未被改写 */
    TEST_ASSERT_EQUAL_INT(0, fakeTotalCalls(&fakeAdc));

    fakeReset(&fakeAdc);
}

/* 用例 9：衰减枚举越界属于非法参数 —— 返回 -1，虚表零调用 */
static void test_invalidAttenuationIsRejected(void)
{
    Adc* adc = NULL;

    fakeReset(&fakeAdc);

    adc = NewAdc(&fakeAdc, fakeInterface());
    TEST_ASSERT_NOT_NULL(adc);

    TEST_ASSERT_EQUAL_INT(ADC_TEST_ERR_PARAM, AdcConfigureChannel(adc, 3u, (AdcAttenuation)99));
    TEST_ASSERT_EQUAL_INT(ADC_TEST_ERR_PARAM, AdcConfigureChannel(adc, 3u, (AdcAttenuation)(-1)));
    TEST_ASSERT_EQUAL_INT(0, fakeTotalCalls(&fakeAdc));

    DeleteAdc(&adc);
    TEST_ASSERT_NULL(adc);

    fakeReset(&fakeAdc);
}

/* 用例 10：NULL 输出指针一律返回 -1，虚表零调用 */
static void test_nullOutputPointerIsRejected(void)
{
    Adc* adc = NULL;

    fakeReset(&fakeAdc);

    adc = NewAdc(&fakeAdc, fakeInterface());
    TEST_ASSERT_NOT_NULL(adc);

    TEST_ASSERT_EQUAL_INT(ADC_TEST_ERR_PARAM, AdcReadRaw(adc, 0u, NULL));
    TEST_ASSERT_EQUAL_INT(ADC_TEST_ERR_PARAM, AdcReadMilliVolts(adc, 0u, NULL));

    TEST_ASSERT_EQUAL_INT(0, fakeTotalCalls(&fakeAdc));

    DeleteAdc(&adc);
    TEST_ASSERT_NULL(adc);

    fakeReset(&fakeAdc);
}

/* 用例 11：AdcConfigureChannel 的 channel / attenuation 原样透传 */
static void test_configureChannelPassThrough(void)
{
    Adc* adc = NULL;

    fakeReset(&fakeAdc);

    adc = NewAdc(&fakeAdc, fakeInterface());
    TEST_ASSERT_NOT_NULL(adc);

    TEST_ASSERT_EQUAL_INT(ADC_TEST_OK, AdcConfigureChannel(adc, 3u, AdcAtten6dB));

    TEST_ASSERT_EQUAL_INT(1, fakeAdc.configureChannelCalls);
    TEST_ASSERT_EQUAL_UINT32(3u, fakeAdc.configuredChannel);
    TEST_ASSERT_EQUAL_INT(AdcAtten6dB, fakeAdc.lastAttenuation);
    TEST_ASSERT_EQUAL_INT(1, fakeTotalCalls(&fakeAdc));

    DeleteAdc(&adc);
    TEST_ASSERT_NULL(adc);

    fakeReset(&fakeAdc);
}

/* 用例 12：AdcReadRaw 把 fake 写入的原始值带出，channel 原样透传 */
static void test_readRawPassThrough(void)
{
    Adc*     adc = NULL;
    uint32_t raw = 0u;

    fakeReset(&fakeAdc);

    fakeAdc.readRawValue  = 4095u;
    fakeAdc.readRawResult = ADC_TEST_OK;

    adc = NewAdc(&fakeAdc, fakeInterface());
    TEST_ASSERT_NOT_NULL(adc);

    TEST_ASSERT_EQUAL_INT(ADC_TEST_OK, AdcReadRaw(adc, 3u, &raw));

    TEST_ASSERT_EQUAL_INT(1, fakeAdc.readRawCalls);
    TEST_ASSERT_EQUAL_UINT32(3u, fakeAdc.readRawChannel);
    TEST_ASSERT_EQUAL_UINT32(4095u, raw);
    TEST_ASSERT_EQUAL_INT(1, fakeTotalCalls(&fakeAdc));

    DeleteAdc(&adc);
    TEST_ASSERT_NULL(adc);

    fakeReset(&fakeAdc);
}

/* 用例 13：AdcReadMilliVolts 把毫伏值带出；-2（校准不可用）原样上报 */
static void test_readMilliVoltsPassThrough(void)
{
    Adc*     adc = NULL;
    uint32_t mv  = 0u;

    fakeReset(&fakeAdc);

    /* 13-a：正常路径，返回值 0 */
    fakeAdc.readMilliVoltsValue  = 1650u;
    fakeAdc.readMilliVoltsResult = ADC_TEST_OK;

    adc = NewAdc(&fakeAdc, fakeInterface());
    TEST_ASSERT_NOT_NULL(adc);

    TEST_ASSERT_EQUAL_INT(ADC_TEST_OK, AdcReadMilliVolts(adc, 2u, &mv));
    TEST_ASSERT_EQUAL_INT(1, fakeAdc.readMilliVoltsCalls);
    TEST_ASSERT_EQUAL_UINT32(2u, fakeAdc.readMilliVoltsChannel);
    TEST_ASSERT_EQUAL_UINT32(1650u, mv);

    /* 13-b：校准不可用 —— 具象层返回 -2，抽象层必须原样上报（不吞、不改写） */
    fakeAdc.readMilliVoltsValue  = 3300u;
    fakeAdc.readMilliVoltsResult = ADC_TEST_ERR_UNSUPPORTED;
    mv                           = 0u;

    TEST_ASSERT_EQUAL_INT(ADC_TEST_ERR_UNSUPPORTED, AdcReadMilliVolts(adc, 4u, &mv));
    TEST_ASSERT_EQUAL_INT(2, fakeAdc.readMilliVoltsCalls);
    TEST_ASSERT_EQUAL_UINT32(4u, fakeAdc.readMilliVoltsChannel);
    TEST_ASSERT_EQUAL_UINT32(3300u, mv);

    TEST_ASSERT_EQUAL_INT(2, fakeTotalCalls(&fakeAdc));

    DeleteAdc(&adc);
    TEST_ASSERT_NULL(adc);

    fakeReset(&fakeAdc);
}

/* 用例 14：虚表业务槽位为 NULL 时返回 -2（未实现 / 不支持） */
static void test_unimplementedInterfaceReturnsUnsupported(void)
{
    Adc*     adc   = NULL;
    uint32_t value = 0u;

    fakeReset(&fakeAdc);

    adc = NewAdc(&fakeAdc, fakeInterfaceWithoutBusiness());
    TEST_ASSERT_NOT_NULL(adc);

    TEST_ASSERT_EQUAL_INT(ADC_TEST_ERR_UNSUPPORTED, AdcInit(adc));
    TEST_ASSERT_EQUAL_INT(ADC_TEST_ERR_UNSUPPORTED, AdcDeinit(adc));
    TEST_ASSERT_EQUAL_INT(ADC_TEST_ERR_UNSUPPORTED, AdcConfigureChannel(adc, 1u, AdcAtten12dB));
    TEST_ASSERT_EQUAL_INT(ADC_TEST_ERR_UNSUPPORTED, AdcReadRaw(adc, 1u, &value));
    TEST_ASSERT_EQUAL_INT(ADC_TEST_ERR_UNSUPPORTED, AdcReadMilliVolts(adc, 1u, &value));

    TEST_ASSERT_EQUAL_INT(0, fakeTotalCalls(&fakeAdc));

    DeleteAdc(&adc);
    TEST_ASSERT_NULL(adc);

    fakeReset(&fakeAdc);
}

/* 用例 15：分配器配平 —— 分配次数 == 释放次数，先恢复钩子再断言 */
static void test_allocatorBalance(void)
{
    void* (*savedMalloc)(size_t) = adcMalloc;
    void  (*savedFree)(void*)    = adcFree;
    Adc*    adc                  = NULL;

    fakeReset(&fakeAdc);

    allocCount = 0;
    freeCount  = 0;
    adcMalloc  = countingMalloc;
    adcFree    = countingFree;

    adc = NewAdc(&fakeAdc, fakeInterface());
    DeleteAdc(&adc);

    /* 先恢复钩子再断言：Unity 断言失败会 longjmp 跳出本用例，
     * 若放在断言之后恢复，失败路径会污染后续用例的分配器。 */
    adcMalloc = savedMalloc;
    adcFree   = savedFree;

    TEST_ASSERT_NULL(adc);
    TEST_ASSERT_EQUAL_INT(1, allocCount);
    TEST_ASSERT_EQUAL_INT(1, freeCount);
    TEST_ASSERT_EQUAL_INT(allocCount, freeCount);
    TEST_ASSERT_EQUAL_INT(1, fakeAdc.deleteCalls);

    allocCount = 0;
    freeCount  = 0;
    fakeReset(&fakeAdc);
}

/* ==========================================================================
 * 4. 本模块测试入口
 * ========================================================================== */
void RunAdcTests(void)
{
    RUN_TEST(test_newAdcRejectsNullInstance);
    RUN_TEST(test_newAdcRejectsMissingDeleter);
    RUN_TEST(test_newAdcAssemblesInstanceAndInterfaces);
    RUN_TEST(test_deleteAdcCallsDeleterOnce);
    RUN_TEST(test_deleteAdcIsNullSafe);
    RUN_TEST(test_defaultConfigFields);
    RUN_TEST(test_initAndDeinitPassThrough);
    RUN_TEST(test_nullHandleIsRejected);
    RUN_TEST(test_invalidAttenuationIsRejected);
    RUN_TEST(test_nullOutputPointerIsRejected);
    RUN_TEST(test_configureChannelPassThrough);
    RUN_TEST(test_readRawPassThrough);
    RUN_TEST(test_readMilliVoltsPassThrough);
    RUN_TEST(test_unimplementedInterfaceReturnsUnsupported);
    RUN_TEST(test_allocatorBalance);
}
