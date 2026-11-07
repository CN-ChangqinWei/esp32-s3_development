/**
 * @file test_pwm.c
 * @brief hal/pwm 抽象父类单元测试：用 FakePwm 具象验证
 *        「抽象构造 / 抽象析构 / 判空护栏 / 参数边界 / 虚表透传 / 分配器配平」。
 *
 * 测试边界：
 *   - 只测 hal/pwm/inc/pwm.h 暴露的抽象层，不触碰任何真实 LEDC 外设；
 *   - 具体实现由本文件内的 FakePwm + fakeInterface() 提供，
 *     不使用 _RETURN_PWM_INTERFACE 宏，避免依赖具象侧的具体函数名；
 *   - 只依赖 unity 与 <stdint.h> / <stdlib.h> / "pwm.h"，不引入任何 ESP-IDF 头。
 *
 * 入口：void RunPwmTests(void);（由 test/main/test_main.c 调用）
 *
 * 用例清单：
 *   1. NewPwm(NULL, ...)                    → NULL，虚表零调用
 *   2. NewPwm(..., deleteInstance == NULL)  → NULL，虚表零调用
 *   3. 正常装配：instance 原样保存、接口表逐项一致
 *   4. DeletePwm 恰好调用 deleteInstance 1 次并把句柄置空
 *   5. DeletePwm(NULL) / DeletePwm(&空指针) 安全
 *   6. 8 个包装函数传 NULL 句柄 → -1，且虚表零调用
 *   7. PwmConfigure 的 config 为 NULL → -1，且虚表零调用
 *   8. PwmConfigure 的 frequencyHz == 0 / resolutionBits == 0 → -1，且虚表零调用
 *   9. PwmSetFrequency 的 frequencyHz == 0 / PwmSetDutyPermille 的 1001 → -1，零调用
 *  10. PwmConfigure 的 channel / frequencyHz / resolutionBits / config 指针原样透传
 *  11. PwmSetDuty 的 channel / duty 原样透传
 *  12. PwmSetDutyPermille 原样透传千分比（抽象层不换算，0 与 1000 边界均放行）
 *  13. PwmStart / PwmStop 的 channel 原样透传
 *  14. 虚表项为 NULL（仅保留析构项）→ -2（未实现 / 不支持）
 *  15. pwmMalloc / pwmFree 计数配平，断言前先恢复原钩子
 *
 * 说明：被测代码语义以 pwm.h 的返回值约定为准，断言统一用
 *       PWM_OK(0) / PWM_ERR_PARAM(-1) / PWM_ERR_UNSUPPORTED(-2)。
 */

#include <stdint.h>
#include <stdlib.h>

#include "unity.h"

#include "pwm.h"

/* 返回值约定（与 pwm.h 注释一致，测试侧本地定义以便断言可读） */
#define PWM_OK                 0
#define PWM_ERR_PARAM        (-1)
#define PWM_ERR_UNSUPPORTED  (-2)

/* 本文件唯一导出符号：由 test/main/test_main.c 声明并调用 */
void RunPwmTests(void);

/* ==========================================================================
 * 1. 测试用具体实现（fake）
 *    只做入参快照与调用计数，绝不访问硬件；所有虚表函数首参为 instance，
 *    体内取 FakePwm* self 后写 self->field。
 * ========================================================================== */

typedef struct FakePwm {
    /* 各虚表函数被调用的次数（用于「恰好 1 次」与「零调用」断言） */
    int initCalls;
    int deinitCalls;
    int configureCalls;
    int setFrequencyCalls;
    int setDutyCalls;
    int setDutyPermilleCalls;
    int startCalls;
    int stopCalls;
    int deleteCalls;

    /* 最近一次调用的入参快照（用于「原样透传」断言） */
    uint32_t         lastChannel;
    const PwmConfig* lastConfig;         /* 校验抽象层未复制 / 未替换 config 指针 */
    uint32_t         lastFrequencyHz;    /* configure 与 setFrequency 共用 */
    uint8_t          lastResolutionBits;
    uint32_t         lastDuty;
    uint32_t         lastPermille;
} FakePwm;

/* 唯一 fake 实例：抽象对象通过 instance 指针持有它 */
static FakePwm fakePwm;

/* --------------------------------------------------------------------------
 * fake 状态复位：每个用例首尾各调用一次，保证用例之间互不影响
 * -------------------------------------------------------------------------- */
static void fakeReset(void)
{
    fakePwm.initCalls            = 0;
    fakePwm.deinitCalls          = 0;
    fakePwm.configureCalls       = 0;
    fakePwm.setFrequencyCalls    = 0;
    fakePwm.setDutyCalls         = 0;
    fakePwm.setDutyPermilleCalls = 0;
    fakePwm.startCalls           = 0;
    fakePwm.stopCalls            = 0;
    fakePwm.deleteCalls          = 0;

    fakePwm.lastChannel        = 0u;
    fakePwm.lastConfig         = NULL;
    fakePwm.lastFrequencyHz    = 0u;
    fakePwm.lastResolutionBits = 0u;
    fakePwm.lastDuty           = 0u;
    fakePwm.lastPermille       = 0u;
}

/* 全部虚表调用次数之和（含 deleteInstance），用于「零调用 / 恰好 1 次」断言 */
static int fakeTotalCalls(void)
{
    return fakePwm.initCalls
         + fakePwm.deinitCalls
         + fakePwm.configureCalls
         + fakePwm.setFrequencyCalls
         + fakePwm.setDutyCalls
         + fakePwm.setDutyPermilleCalls
         + fakePwm.startCalls
         + fakePwm.stopCalls
         + fakePwm.deleteCalls;
}

/* --------------------------------------------------------------------------
 * fake 虚表实现
 * -------------------------------------------------------------------------- */

static int fakeInit(void* instance)
{
    FakePwm* self = (FakePwm*)instance;

    if (self == NULL) {
        return PWM_ERR_PARAM;
    }

    self->initCalls++;

    return PWM_OK;
}

static int fakeDeinit(void* instance)
{
    FakePwm* self = (FakePwm*)instance;

    if (self == NULL) {
        return PWM_ERR_PARAM;
    }

    self->deinitCalls++;

    return PWM_OK;
}

static int fakeConfigure(void* instance, uint32_t channel, const PwmConfig* config)
{
    FakePwm* self = (FakePwm*)instance;

    if (self == NULL || config == NULL) {
        return PWM_ERR_PARAM;
    }

    self->configureCalls++;
    self->lastChannel        = channel;
    self->lastConfig         = config;
    self->lastFrequencyHz    = config->frequencyHz;
    self->lastResolutionBits = config->resolutionBits;

    return PWM_OK;
}

static int fakeSetFrequency(void* instance, uint32_t channel, uint32_t frequencyHz)
{
    FakePwm* self = (FakePwm*)instance;

    if (self == NULL) {
        return PWM_ERR_PARAM;
    }

    self->setFrequencyCalls++;
    self->lastChannel     = channel;
    self->lastFrequencyHz = frequencyHz;

    return PWM_OK;
}

static int fakeSetDuty(void* instance, uint32_t channel, uint32_t duty)
{
    FakePwm* self = (FakePwm*)instance;

    if (self == NULL) {
        return PWM_ERR_PARAM;
    }

    self->setDutyCalls++;
    self->lastChannel = channel;
    self->lastDuty    = duty;

    return PWM_OK;
}

static int fakeSetDutyPermille(void* instance, uint32_t channel, uint32_t permille)
{
    FakePwm* self = (FakePwm*)instance;

    if (self == NULL) {
        return PWM_ERR_PARAM;
    }

    self->setDutyPermilleCalls++;
    self->lastChannel  = channel;
    self->lastPermille = permille;

    return PWM_OK;
}

static int fakeStart(void* instance, uint32_t channel)
{
    FakePwm* self = (FakePwm*)instance;

    if (self == NULL) {
        return PWM_ERR_PARAM;
    }

    self->startCalls++;
    self->lastChannel = channel;

    return PWM_OK;
}

static int fakeStop(void* instance, uint32_t channel)
{
    FakePwm* self = (FakePwm*)instance;

    if (self == NULL) {
        return PWM_ERR_PARAM;
    }

    self->stopCalls++;
    self->lastChannel = channel;

    return PWM_OK;
}

static void fakeDeleteInstance(void* instance)
{
    FakePwm* self = (FakePwm*)instance;

    if (self == NULL) {
        return;
    }

    self->deleteCalls++;
}

/* --------------------------------------------------------------------------
 * 接口表：聚合初始化（不使用 _RETURN_PWM_INTERFACE / _WITH 宏）
 * -------------------------------------------------------------------------- */
static PwmInterface fakeInterface(void)
{
    PwmInterface interfaces = {
        .init            = fakeInit,
        .deinit          = fakeDeinit,
        .configure       = fakeConfigure,
        .setFrequency    = fakeSetFrequency,
        .setDuty         = fakeSetDuty,
        .setDutyPermille = fakeSetDutyPermille,
        .start           = fakeStart,
        .stop            = fakeStop,
        .deleteInstance  = fakeDeleteInstance,
    };

    return interfaces;
}

/* 变体：析构项缺失，用于验证抽象构造的入参校验 */
static PwmInterface fakeInterfaceWithoutDeleter(void)
{
    PwmInterface interfaces = fakeInterface();

    interfaces.deleteInstance = NULL;

    return interfaces;
}

/* 变体：除析构项外全部未实现，用于验证包装函数的 -2 分支 */
static PwmInterface fakeInterfaceWithoutImplementations(void)
{
    PwmInterface interfaces = {
        .init            = NULL,
        .deinit          = NULL,
        .configure       = NULL,
        .setFrequency    = NULL,
        .setDuty         = NULL,
        .setDutyPermille = NULL,
        .start           = NULL,
        .stop            = NULL,
        .deleteInstance  = fakeDeleteInstance,
    };

    return interfaces;
}

/* 两张接口表是否逐项一致（函数指针同类型比较，可直接 ==） */
static int interfacesEqual(PwmInterface left, PwmInterface right)
{
    if (left.init != right.init) {
        return 0;
    }
    if (left.deinit != right.deinit) {
        return 0;
    }
    if (left.configure != right.configure) {
        return 0;
    }
    if (left.setFrequency != right.setFrequency) {
        return 0;
    }
    if (left.setDuty != right.setDuty) {
        return 0;
    }
    if (left.setDutyPermille != right.setDutyPermille) {
        return 0;
    }
    if (left.start != right.start) {
        return 0;
    }
    if (left.stop != right.stop) {
        return 0;
    }
    if (left.deleteInstance != right.deleteInstance) {
        return 0;
    }

    return 1;
}

/* --------------------------------------------------------------------------
 * 断言辅助：1 表示 NewPwm 按契约拒绝了非法入参（返回 NULL）；
 *            0 表示未拒绝 —— 顺手回收对象，避免用例失败时泄漏内存。
 * -------------------------------------------------------------------------- */
static int newPwmRejected(Pwm* pwm)
{
    if (pwm == NULL) {
        return 1;
    }

    DeletePwm(&pwm);

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
static void test_newPwmRejectsNullInstance(void)
{
    fakeReset();

    TEST_ASSERT_TRUE_MESSAGE(newPwmRejected(NewPwm(NULL, fakeInterface())),
                             "NewPwm 必须拒绝 NULL instance 并返回 NULL");
    TEST_ASSERT_EQUAL_INT(0, fakeTotalCalls());

    fakeReset();
}

/* 用例 2：析构项缺失时必须拒绝装配（否则 DeletePwm 无法销毁具体实例） */
static void test_newPwmRejectsMissingDeleter(void)
{
    fakeReset();

    TEST_ASSERT_TRUE_MESSAGE(newPwmRejected(NewPwm(&fakePwm, fakeInterfaceWithoutDeleter())),
                             "NewPwm 必须拒绝 deleteInstance 为 NULL 的接口表");
    TEST_ASSERT_EQUAL_INT(0, fakeTotalCalls());

    fakeReset();
}

/* 用例 3：正常装配 —— instance 原样保存、接口表逐项一致 */
static void test_newPwmAssemblesInstanceAndInterfaces(void)
{
    Pwm*         pwm = NULL;
    PwmInterface expected;

    fakeReset();
    expected = fakeInterface();

    pwm = NewPwm(&fakePwm, expected);
    TEST_ASSERT_NOT_NULL(pwm);

    TEST_ASSERT_EQUAL_PTR(&fakePwm, pwm->instance);
    TEST_ASSERT_TRUE_MESSAGE(interfacesEqual(expected, pwm->interfaces),
                             "抽象对象必须原样保存接口表");
    TEST_ASSERT_EQUAL_INT(0, fakeTotalCalls());

    DeletePwm(&pwm);
    TEST_ASSERT_NULL(pwm);

    fakeReset();
}

/* 用例 4：析构恰好调用 deleteInstance 1 次，并把句柄置空 */
static void test_deletePwmCallsDeleterOnce(void)
{
    Pwm* pwm = NULL;

    fakeReset();

    pwm = NewPwm(&fakePwm, fakeInterface());
    TEST_ASSERT_NOT_NULL(pwm);

    DeletePwm(&pwm);

    TEST_ASSERT_EQUAL_INT(1, fakePwm.deleteCalls);
    TEST_ASSERT_EQUAL_INT(1, fakeTotalCalls());
    TEST_ASSERT_NULL(pwm);

    fakeReset();
}

/* 用例 5：DeletePwm 的空入参安全，且不触达虚表 */
static void test_deletePwmIsNullSafe(void)
{
    Pwm* pwm = NULL;

    fakeReset();

    DeletePwm(NULL);   /* 二级指针为 NULL：不崩溃即可 */
    DeletePwm(&pwm);   /* *pPwm 为 NULL：不崩溃，且仍为 NULL */

    TEST_ASSERT_NULL(pwm);
    TEST_ASSERT_EQUAL_INT(0, fakeTotalCalls());

    fakeReset();
}

/* 用例 6：8 个包装函数传 NULL 句柄一律返回 -1，虚表零调用 */
static void test_nullHandleIsRejected(void)
{
    PwmConfig config;

    config.frequencyHz    = 1000u;
    config.resolutionBits = 8u;

    fakeReset();

    TEST_ASSERT_EQUAL_INT(PWM_ERR_PARAM, PwmInit(NULL));
    TEST_ASSERT_EQUAL_INT(PWM_ERR_PARAM, PwmDeinit(NULL));
    TEST_ASSERT_EQUAL_INT(PWM_ERR_PARAM, PwmConfigure(NULL, 0u, &config));
    TEST_ASSERT_EQUAL_INT(PWM_ERR_PARAM, PwmSetFrequency(NULL, 0u, 1000u));
    TEST_ASSERT_EQUAL_INT(PWM_ERR_PARAM, PwmSetDuty(NULL, 0u, 128u));
    TEST_ASSERT_EQUAL_INT(PWM_ERR_PARAM, PwmSetDutyPermille(NULL, 0u, 500u));
    TEST_ASSERT_EQUAL_INT(PWM_ERR_PARAM, PwmStart(NULL, 0u));
    TEST_ASSERT_EQUAL_INT(PWM_ERR_PARAM, PwmStop(NULL, 0u));

    TEST_ASSERT_EQUAL_INT(0, fakeTotalCalls());

    fakeReset();
}

/* 用例 7：config 为 NULL → -1，虚表零调用 */
static void test_nullConfigIsRejected(void)
{
    Pwm* pwm = NULL;

    fakeReset();

    pwm = NewPwm(&fakePwm, fakeInterface());
    TEST_ASSERT_NOT_NULL(pwm);

    TEST_ASSERT_EQUAL_INT(PWM_ERR_PARAM, PwmConfigure(pwm, 0u, NULL));
    TEST_ASSERT_EQUAL_INT(0, fakeTotalCalls());

    DeletePwm(&pwm);
    TEST_ASSERT_NULL(pwm);

    fakeReset();
}

/* 用例 8：frequencyHz == 0 / resolutionBits == 0 → -1，虚表零调用 */
static void test_invalidConfigValuesAreRejected(void)
{
    Pwm*      pwm = NULL;
    PwmConfig config;

    fakeReset();

    pwm = NewPwm(&fakePwm, fakeInterface());
    TEST_ASSERT_NOT_NULL(pwm);

    /* 频率为 0：抽象层直接拒绝 */
    config.frequencyHz    = 0u;
    config.resolutionBits = 8u;
    TEST_ASSERT_EQUAL_INT(PWM_ERR_PARAM, PwmConfigure(pwm, 2u, &config));

    /* 分辨率为 0：抽象层直接拒绝（上限 14 由具象层判定，此处不测上限） */
    config.frequencyHz    = 1000u;
    config.resolutionBits = 0u;
    TEST_ASSERT_EQUAL_INT(PWM_ERR_PARAM, PwmConfigure(pwm, 2u, &config));

    TEST_ASSERT_EQUAL_INT(0, fakeTotalCalls());

    DeletePwm(&pwm);
    TEST_ASSERT_NULL(pwm);

    fakeReset();
}

/* 用例 9：setFrequency 频率为 0、千分比越界 → -1，虚表零调用 */
static void test_invalidFrequencyAndPermilleAreRejected(void)
{
    Pwm* pwm = NULL;

    fakeReset();

    pwm = NewPwm(&fakePwm, fakeInterface());
    TEST_ASSERT_NOT_NULL(pwm);

    TEST_ASSERT_EQUAL_INT(PWM_ERR_PARAM, PwmSetFrequency(pwm, 1u, 0u));
    TEST_ASSERT_EQUAL_INT(PWM_ERR_PARAM, PwmSetDutyPermille(pwm, 1u, 1001u));

    TEST_ASSERT_EQUAL_INT(0, fakeTotalCalls());

    DeletePwm(&pwm);
    TEST_ASSERT_NULL(pwm);

    fakeReset();
}

/* 用例 10：PwmConfigure 的 channel / 频率 / 分辨率 / config 指针原样透传 */
static void test_configurePassThrough(void)
{
    Pwm*      pwm = NULL;
    PwmConfig config;

    fakeReset();

    pwm = NewPwm(&fakePwm, fakeInterface());
    TEST_ASSERT_NOT_NULL(pwm);

    config.frequencyHz    = 20000u;
    config.resolutionBits = 10u;

    TEST_ASSERT_EQUAL_INT(PWM_OK, PwmConfigure(pwm, 2u, &config));

    TEST_ASSERT_EQUAL_INT(1, fakePwm.configureCalls);
    TEST_ASSERT_EQUAL_UINT32(2u, fakePwm.lastChannel);
    TEST_ASSERT_EQUAL_UINT32(config.frequencyHz, fakePwm.lastFrequencyHz);
    TEST_ASSERT_EQUAL_UINT8(config.resolutionBits, fakePwm.lastResolutionBits);
    TEST_ASSERT_TRUE_MESSAGE(fakePwm.lastConfig == &config,
                             "抽象层必须原样透传 config 指针，不得复制或替换");
    TEST_ASSERT_EQUAL_INT(1, fakeTotalCalls());

    DeletePwm(&pwm);
    TEST_ASSERT_NULL(pwm);

    fakeReset();
}

/* 用例 11：PwmSetDuty 的 channel / duty 原样透传（不校验上限，上限归具象层） */
static void test_setDutyPassThrough(void)
{
    Pwm* pwm = NULL;

    fakeReset();

    pwm = NewPwm(&fakePwm, fakeInterface());
    TEST_ASSERT_NOT_NULL(pwm);

    TEST_ASSERT_EQUAL_INT(PWM_OK, PwmSetDuty(pwm, 3u, 1234u));

    TEST_ASSERT_EQUAL_INT(1, fakePwm.setDutyCalls);
    TEST_ASSERT_EQUAL_UINT32(3u, fakePwm.lastChannel);
    TEST_ASSERT_EQUAL_UINT32(1234u, fakePwm.lastDuty);
    TEST_ASSERT_EQUAL_INT(1, fakeTotalCalls());

    DeletePwm(&pwm);
    TEST_ASSERT_NULL(pwm);

    fakeReset();
}

/* 用例 12：PwmSetDutyPermille 原样透传千分比 —— 抽象层不做换算 */
static void test_setDutyPermillePassThrough(void)
{
    Pwm* pwm = NULL;

    fakeReset();

    pwm = NewPwm(&fakePwm, fakeInterface());
    TEST_ASSERT_NOT_NULL(pwm);

    /* 500‰ 必须原样到达具象层（若抽象层擅自换算，这里会变成 2047 之类） */
    TEST_ASSERT_EQUAL_INT(PWM_OK, PwmSetDutyPermille(pwm, 1u, 500u));
    TEST_ASSERT_EQUAL_INT(1, fakePwm.setDutyPermilleCalls);
    TEST_ASSERT_EQUAL_UINT32(1u, fakePwm.lastChannel);
    TEST_ASSERT_EQUAL_UINT32(500u, fakePwm.lastPermille);

    /* 边界：0 与 1000 均合法（校验条件是 permille > 1000） */
    TEST_ASSERT_EQUAL_INT(PWM_OK, PwmSetDutyPermille(pwm, 1u, 0u));
    TEST_ASSERT_EQUAL_UINT32(0u, fakePwm.lastPermille);

    TEST_ASSERT_EQUAL_INT(PWM_OK, PwmSetDutyPermille(pwm, 1u, 1000u));
    TEST_ASSERT_EQUAL_UINT32(1000u, fakePwm.lastPermille);

    TEST_ASSERT_EQUAL_INT(3, fakePwm.setDutyPermilleCalls);
    TEST_ASSERT_EQUAL_INT(3, fakeTotalCalls());

    DeletePwm(&pwm);
    TEST_ASSERT_NULL(pwm);

    fakeReset();
}

/* 用例 13：PwmStart / PwmStop 的 channel 原样透传 */
static void test_startStopPassThrough(void)
{
    Pwm* pwm = NULL;

    fakeReset();

    pwm = NewPwm(&fakePwm, fakeInterface());
    TEST_ASSERT_NOT_NULL(pwm);

    TEST_ASSERT_EQUAL_INT(PWM_OK, PwmStart(pwm, 4u));
    TEST_ASSERT_EQUAL_INT(1, fakePwm.startCalls);
    TEST_ASSERT_EQUAL_UINT32(4u, fakePwm.lastChannel);

    TEST_ASSERT_EQUAL_INT(PWM_OK, PwmStop(pwm, 5u));
    TEST_ASSERT_EQUAL_INT(1, fakePwm.stopCalls);
    TEST_ASSERT_EQUAL_UINT32(5u, fakePwm.lastChannel);

    TEST_ASSERT_EQUAL_INT(2, fakeTotalCalls());

    DeletePwm(&pwm);
    TEST_ASSERT_NULL(pwm);

    fakeReset();
}

/* 用例 14：虚表项为 NULL 时返回 -2（未实现 / 不支持） */
static void test_unimplementedInterfaceReturnsUnsupported(void)
{
    Pwm*      pwm = NULL;
    PwmConfig config;

    config.frequencyHz    = 1000u;
    config.resolutionBits = 8u;

    fakeReset();

    pwm = NewPwm(&fakePwm, fakeInterfaceWithoutImplementations());
    TEST_ASSERT_NOT_NULL(pwm);

    TEST_ASSERT_EQUAL_INT(PWM_ERR_UNSUPPORTED, PwmInit(pwm));
    TEST_ASSERT_EQUAL_INT(PWM_ERR_UNSUPPORTED, PwmDeinit(pwm));
    TEST_ASSERT_EQUAL_INT(PWM_ERR_UNSUPPORTED, PwmConfigure(pwm, 0u, &config));
    TEST_ASSERT_EQUAL_INT(PWM_ERR_UNSUPPORTED, PwmSetFrequency(pwm, 0u, 1000u));
    TEST_ASSERT_EQUAL_INT(PWM_ERR_UNSUPPORTED, PwmSetDuty(pwm, 0u, 128u));
    TEST_ASSERT_EQUAL_INT(PWM_ERR_UNSUPPORTED, PwmSetDutyPermille(pwm, 0u, 500u));
    TEST_ASSERT_EQUAL_INT(PWM_ERR_UNSUPPORTED, PwmStart(pwm, 0u));
    TEST_ASSERT_EQUAL_INT(PWM_ERR_UNSUPPORTED, PwmStop(pwm, 0u));

    TEST_ASSERT_EQUAL_INT(0, fakeTotalCalls());

    DeletePwm(&pwm);
    TEST_ASSERT_NULL(pwm);

    fakeReset();
}

/* 用例 15：分配器配平 —— 分配次数 == 释放次数，用完恢复原钩子 */
static void test_allocatorBalance(void)
{
    void* (*savedMalloc)(size_t) = pwmMalloc;
    void  (*savedFree)(void*)    = pwmFree;
    Pwm*    pwm                  = NULL;

    fakeReset();

    allocCount = 0;
    freeCount  = 0;
    pwmMalloc  = countingMalloc;
    pwmFree    = countingFree;

    pwm = NewPwm(&fakePwm, fakeInterface());
    DeletePwm(&pwm);

    /* 先恢复钩子再断言：Unity 断言失败会 longjmp 跳出本用例，
     * 若放在断言之后恢复，失败路径会污染后续用例的分配器。 */
    pwmMalloc = savedMalloc;
    pwmFree   = savedFree;

    TEST_ASSERT_NULL(pwm);
    TEST_ASSERT_EQUAL_INT(1, allocCount);
    TEST_ASSERT_EQUAL_INT(1, freeCount);
    TEST_ASSERT_EQUAL_INT(allocCount, freeCount);
    TEST_ASSERT_EQUAL_INT(1, fakePwm.deleteCalls);

    allocCount = 0;
    freeCount  = 0;
    fakeReset();
}

/* ==========================================================================
 * 4. 本模块测试入口
 * ========================================================================== */
void RunPwmTests(void)
{
    RUN_TEST(test_newPwmRejectsNullInstance);
    RUN_TEST(test_newPwmRejectsMissingDeleter);
    RUN_TEST(test_newPwmAssemblesInstanceAndInterfaces);
    RUN_TEST(test_deletePwmCallsDeleterOnce);
    RUN_TEST(test_deletePwmIsNullSafe);
    RUN_TEST(test_nullHandleIsRejected);
    RUN_TEST(test_nullConfigIsRejected);
    RUN_TEST(test_invalidConfigValuesAreRejected);
    RUN_TEST(test_invalidFrequencyAndPermilleAreRejected);
    RUN_TEST(test_configurePassThrough);
    RUN_TEST(test_setDutyPassThrough);
    RUN_TEST(test_setDutyPermillePassThrough);
    RUN_TEST(test_startStopPassThrough);
    RUN_TEST(test_unimplementedInterfaceReturnsUnsupported);
    RUN_TEST(test_allocatorBalance);
}
