/**
 * @file test_gpio.c
 * @brief hal/gpio 抽象父类单元测试：用 FakeGpio 具象验证
 *        「抽象构造 / 抽象析构 / 判空护栏 / 虚表透传 / 内存钩子配平」。
 *
 * 测试边界：
 *   - 只测 hal/gpio/inc/gpio.h 暴露的抽象层，不触碰任何真实外设；
 *   - 具体实现由本文件内的 FakeGpio + fakeInterface() 提供，
 *     不使用 _RETURN_GPIO_INTERFACE 宏，避免依赖具象侧的具体函数名；
 *   - 只依赖 unity 与 <stdint.h> / <stdlib.h>，不引入任何 ESP-IDF 头。
 *
 * 入口：void RunGpioTests(void);（由 test/main/test_main.c 调用）
 *
 * 用例清单：
 *   1. NewGpio(NULL, ...)               → NULL
 *   2. NewGpio(..., deleteInstance==NULL) → NULL
 *   3. 正常装配：instance 原样保存、接口表逐项一致
 *   4. DeleteGpio 恰好调用 deleteInstance 1 次并置空句柄
 *   5. DeleteGpio(NULL) / DeleteGpio(&空指针) 安全
 *   6. 8 个包装函数传 NULL 句柄 → -1，且虚表零调用
 *   7. GpioRead / GpioAttachInterrupt 输出指针 / 回调为 NULL → -1，且虚表零调用
 *   8. GpioWrite 的 pin / level 原样透传
 *   9. GpioRead 把 fake 的电平带出，pin 原样透传
 *  10. GpioAttachInterrupt 的 edge / callback / context 原样透传
 *  11. 接口未实现（虚表项为 NULL）→ -2
 *  12. gpioMalloc / gpioFree 计数配平，用例结束恢复原钩子
 *
 * 说明：被测代码语义以 gpio.h 的返回值约定为准，断言统一用
 *       GPIO_OK(0) / GPIO_ERR_PARAM(-1) / GPIO_ERR_UNSUPPORTED(-2)。
 */

#include <stdint.h>
#include <stdlib.h>

#include "unity.h"

#include "gpio.h"

/* 本文件唯一导出符号：由 test/main/test_main.c 声明并调用 */
void RunGpioTests(void);

/* ==========================================================================
 * 1. 测试用具体实现（fake）
 *    只做入参快照与计数，绝不访问硬件；所有虚表函数首参为 instance，
 *    体内取 FakeGpio* self 后写 self->field。
 * ========================================================================== */

typedef struct FakeGpio {
    /* 各虚表函数被调用的次数（用于「恰好 1 次」与「零调用」断言） */
    int initCalls;
    int deinitCalls;
    int setDirectionCalls;
    int writeCalls;
    int readCalls;
    int toggleCalls;
    int attachCalls;
    int detachCalls;
    int deleteCalls;

    /* 最近一次调用的入参快照（用于「原样透传」断言） */
    uint32_t        lastPin;
    GpioDirection   lastDirection;
    GpioPull        lastPull;
    GpioLevel       lastLevel;
    GpioEdge        lastEdge;
    GpioIsrCallback lastCallback;
    void*           lastContext;

    /* read 的返回码与要带出给抽象层的电平 */
    int       readResult;
    GpioLevel readLevel;
} FakeGpio;

/* 唯一 fake 实例：抽象对象通过 instance 指针持有它 */
static FakeGpio fakeGpio;

/* 中断回调被真正触发时的记录（独立于虚表计数，不计入 totalCalls） */
static int      isrFireCount;
static uint32_t isrFirePin;

/* --------------------------------------------------------------------------
 * fake 状态复位：每个用例首尾各调用一次，保证用例之间互不影响
 * -------------------------------------------------------------------------- */
static void fakeReset(FakeGpio* self)
{
    if (self == NULL) {
        return;
    }

    self->initCalls         = 0;
    self->deinitCalls       = 0;
    self->setDirectionCalls = 0;
    self->writeCalls        = 0;
    self->readCalls         = 0;
    self->toggleCalls       = 0;
    self->attachCalls       = 0;
    self->detachCalls       = 0;
    self->deleteCalls       = 0;

    self->lastPin      = 0u;
    self->lastDirection = GpioDirInput;
    self->lastPull     = GpioPullNone;
    self->lastLevel    = GpioLevelLow;
    self->lastEdge     = GpioEdgeNone;
    self->lastCallback = NULL;
    self->lastContext  = NULL;

    self->readResult = GPIO_OK;
    self->readLevel  = GpioLevelLow;

    isrFireCount = 0;
    isrFirePin   = 0u;
}

/* 全部虚表调用次数之和（含 deleteInstance），用于「零调用 / 恰好 1 次」断言 */
static int fakeTotalCalls(const FakeGpio* self)
{
    if (self == NULL) {
        return -1;
    }

    return self->initCalls
         + self->deinitCalls
         + self->setDirectionCalls
         + self->writeCalls
         + self->readCalls
         + self->toggleCalls
         + self->attachCalls
         + self->detachCalls
         + self->deleteCalls;
}

/* --------------------------------------------------------------------------
 * fake 虚表实现
 * -------------------------------------------------------------------------- */

/* 供 GpioAttachInterrupt 透传用的回调：只记录触发次数与引脚，不解引用 context */
static void fakeIsrCallback(void* context, uint32_t pin)
{
    (void)context;  /* context 由用例通过 lastContext 单独校验 */

    isrFireCount++;
    isrFirePin = pin;
}

static int fakeInit(void* instance, uint32_t pin, GpioDirection direction, GpioPull pull)
{
    FakeGpio* self = (FakeGpio*)instance;

    if (self == NULL) {
        return GPIO_ERR_PARAM;
    }

    self->initCalls++;
    self->lastPin       = pin;
    self->lastDirection = direction;
    self->lastPull      = pull;

    return GPIO_OK;
}

static int fakeDeinit(void* instance, uint32_t pin)
{
    FakeGpio* self = (FakeGpio*)instance;

    if (self == NULL) {
        return GPIO_ERR_PARAM;
    }

    self->deinitCalls++;
    self->lastPin = pin;

    return GPIO_OK;
}

static int fakeSetDirection(void* instance, uint32_t pin, GpioDirection direction)
{
    FakeGpio* self = (FakeGpio*)instance;

    if (self == NULL) {
        return GPIO_ERR_PARAM;
    }

    self->setDirectionCalls++;
    self->lastPin       = pin;
    self->lastDirection = direction;

    return GPIO_OK;
}

static int fakeWrite(void* instance, uint32_t pin, GpioLevel level)
{
    FakeGpio* self = (FakeGpio*)instance;

    if (self == NULL) {
        return GPIO_ERR_PARAM;
    }

    self->writeCalls++;
    self->lastPin   = pin;
    self->lastLevel = level;

    return GPIO_OK;
}

static int fakeRead(void* instance, uint32_t pin, GpioLevel* level)
{
    FakeGpio* self = (FakeGpio*)instance;

    if (self == NULL || level == NULL) {
        return GPIO_ERR_PARAM;
    }

    self->readCalls++;
    self->lastPin = pin;
    *level        = self->readLevel;

    return self->readResult;
}

static int fakeToggle(void* instance, uint32_t pin)
{
    FakeGpio* self = (FakeGpio*)instance;

    if (self == NULL) {
        return GPIO_ERR_PARAM;
    }

    self->toggleCalls++;
    self->lastPin = pin;

    return GPIO_OK;
}

static int fakeAttachInterrupt(void* instance, uint32_t pin, GpioEdge edge,
                               GpioIsrCallback callback, void* context)
{
    FakeGpio* self = (FakeGpio*)instance;

    if (self == NULL) {
        return GPIO_ERR_PARAM;
    }

    self->attachCalls++;
    self->lastPin      = pin;
    self->lastEdge     = edge;
    self->lastCallback = callback;
    self->lastContext  = context;

    return GPIO_OK;
}

static int fakeDetachInterrupt(void* instance, uint32_t pin)
{
    FakeGpio* self = (FakeGpio*)instance;

    if (self == NULL) {
        return GPIO_ERR_PARAM;
    }

    self->detachCalls++;
    self->lastPin = pin;

    return GPIO_OK;
}

static void fakeDeleteInstance(void* instance)
{
    FakeGpio* self = (FakeGpio*)instance;

    if (self == NULL) {
        return;
    }

    self->deleteCalls++;
}

/* --------------------------------------------------------------------------
 * 接口表：聚合初始化（不使用 _RETURN_GPIO_INTERFACE 宏）
 * -------------------------------------------------------------------------- */
static GpioInterface fakeInterface(void)
{
    GpioInterface interfaces = {
        .init            = fakeInit,
        .deinit          = fakeDeinit,
        .setDirection    = fakeSetDirection,
        .write           = fakeWrite,
        .read            = fakeRead,
        .toggle          = fakeToggle,
        .attachInterrupt = fakeAttachInterrupt,
        .detachInterrupt = fakeDetachInterrupt,
        .deleteInstance  = fakeDeleteInstance,
    };

    return interfaces;
}

/* 变体：析构项缺失，用于验证抽象构造的入参校验 */
static GpioInterface fakeInterfaceWithoutDeleter(void)
{
    GpioInterface interfaces = fakeInterface();

    interfaces.deleteInstance = NULL;

    return interfaces;
}

/* 变体：init 未实现，用于验证包装函数的 -2 分支 */
static GpioInterface fakeInterfaceWithoutInit(void)
{
    GpioInterface interfaces = fakeInterface();

    interfaces.init = NULL;

    return interfaces;
}

/* 两张接口表是否逐项一致（函数指针同类型比较，可直接 ==） */
static int interfacesEqual(GpioInterface left, GpioInterface right)
{
    if (left.init != right.init) {
        return 0;
    }
    if (left.deinit != right.deinit) {
        return 0;
    }
    if (left.setDirection != right.setDirection) {
        return 0;
    }
    if (left.write != right.write) {
        return 0;
    }
    if (left.read != right.read) {
        return 0;
    }
    if (left.toggle != right.toggle) {
        return 0;
    }
    if (left.attachInterrupt != right.attachInterrupt) {
        return 0;
    }
    if (left.detachInterrupt != right.detachInterrupt) {
        return 0;
    }
    if (left.deleteInstance != right.deleteInstance) {
        return 0;
    }

    return 1;
}

/* --------------------------------------------------------------------------
 * 断言辅助：1 表示 NewGpio 按契约拒绝了非法入参（返回 NULL）；
 *            0 表示未拒绝 —— 顺手回收对象，避免用例失败时泄漏内存。
 * -------------------------------------------------------------------------- */
static int newGpioRejected(Gpio* gpio)
{
    if (gpio == NULL) {
        return 1;
    }

    DeleteGpio(&gpio);

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
static void test_newGpioRejectsNullInstance(void)
{
    fakeReset(&fakeGpio);

    TEST_ASSERT_TRUE_MESSAGE(newGpioRejected(NewGpio(NULL, fakeInterface())),
                             "NewGpio 必须拒绝 NULL instance 并返回 NULL");
    TEST_ASSERT_EQUAL_INT(0, fakeTotalCalls(&fakeGpio));

    fakeReset(&fakeGpio);
}

/* 用例 2：析构项缺失时必须拒绝装配（否则 DeleteGpio 无法销毁具体实例） */
static void test_newGpioRejectsMissingDeleter(void)
{
    fakeReset(&fakeGpio);

    TEST_ASSERT_TRUE_MESSAGE(newGpioRejected(NewGpio(&fakeGpio, fakeInterfaceWithoutDeleter())),
                             "NewGpio 必须拒绝 deleteInstance 为 NULL 的接口表");
    TEST_ASSERT_EQUAL_INT(0, fakeTotalCalls(&fakeGpio));

    fakeReset(&fakeGpio);
}

/* 用例 3：正常装配 —— instance 原样保存、接口表逐项一致 */
static void test_newGpioAssemblesInstanceAndInterfaces(void)
{
    Gpio*         gpio = NULL;
    GpioInterface expected;

    fakeReset(&fakeGpio);
    expected = fakeInterface();

    gpio = NewGpio(&fakeGpio, expected);
    TEST_ASSERT_NOT_NULL(gpio);

    TEST_ASSERT_EQUAL_PTR(&fakeGpio, gpio->instance);
    TEST_ASSERT_TRUE_MESSAGE(interfacesEqual(expected, gpio->interfaces),
                             "抽象对象必须原样保存接口表");
    TEST_ASSERT_EQUAL_INT(0, fakeTotalCalls(&fakeGpio));

    DeleteGpio(&gpio);
    TEST_ASSERT_NULL(gpio);

    fakeReset(&fakeGpio);
}

/* 用例 4：析构恰好调用 deleteInstance 1 次，并把句柄置空 */
static void test_deleteGpioCallsDeleterOnce(void)
{
    Gpio* gpio = NULL;

    fakeReset(&fakeGpio);

    gpio = NewGpio(&fakeGpio, fakeInterface());
    TEST_ASSERT_NOT_NULL(gpio);

    DeleteGpio(&gpio);

    TEST_ASSERT_EQUAL_INT(1, fakeGpio.deleteCalls);
    TEST_ASSERT_EQUAL_INT(1, fakeTotalCalls(&fakeGpio));
    TEST_ASSERT_NULL(gpio);

    fakeReset(&fakeGpio);
}

/* 用例 5：DeleteGpio 的空入参安全 */
static void test_deleteGpioIsNullSafe(void)
{
    Gpio* gpio = NULL;

    fakeReset(&fakeGpio);

    DeleteGpio(NULL);   /* 二级指针为 NULL：不崩溃即可 */
    DeleteGpio(&gpio);  /* *pGpio 为 NULL：不崩溃，且仍为 NULL */

    TEST_ASSERT_NULL(gpio);
    TEST_ASSERT_EQUAL_INT(0, fakeTotalCalls(&fakeGpio));

    fakeReset(&fakeGpio);
}

/* 用例 6：8 个包装函数传 NULL 句柄一律返回 -1，虚表零调用 */
static void test_nullHandleIsRejected(void)
{
    GpioLevel level = GpioLevelLow;

    fakeReset(&fakeGpio);

    TEST_ASSERT_EQUAL_INT(GPIO_ERR_PARAM, GpioInit(NULL, 0u, GpioDirOutput, GpioPullNone));
    TEST_ASSERT_EQUAL_INT(GPIO_ERR_PARAM, GpioDeinit(NULL, 0u));
    TEST_ASSERT_EQUAL_INT(GPIO_ERR_PARAM, GpioSetDirection(NULL, 0u, GpioDirOutput));
    TEST_ASSERT_EQUAL_INT(GPIO_ERR_PARAM, GpioWrite(NULL, 0u, GpioLevelHigh));
    TEST_ASSERT_EQUAL_INT(GPIO_ERR_PARAM, GpioRead(NULL, 0u, &level));
    TEST_ASSERT_EQUAL_INT(GPIO_ERR_PARAM, GpioToggle(NULL, 0u));
    TEST_ASSERT_EQUAL_INT(GPIO_ERR_PARAM,
                          GpioAttachInterrupt(NULL, 0u, GpioEdgeAny, fakeIsrCallback, &level));
    TEST_ASSERT_EQUAL_INT(GPIO_ERR_PARAM, GpioDetachInterrupt(NULL, 0u));

    TEST_ASSERT_EQUAL_INT(GpioLevelLow, level);   /* 输出参数未被改写 */
    TEST_ASSERT_EQUAL_INT(0, fakeTotalCalls(&fakeGpio));

    fakeReset(&fakeGpio);
}

/* 用例 7：NULL 输出指针 / NULL 回调一律返回 -1，虚表零调用 */
static void test_nullOutputPointerIsRejected(void)
{
    Gpio* gpio = NULL;

    fakeReset(&fakeGpio);

    gpio = NewGpio(&fakeGpio, fakeInterface());
    TEST_ASSERT_NOT_NULL(gpio);

    TEST_ASSERT_EQUAL_INT(GPIO_ERR_PARAM, GpioRead(gpio, 7u, NULL));
    TEST_ASSERT_EQUAL_INT(GPIO_ERR_PARAM,
                          GpioAttachInterrupt(gpio, 7u, GpioEdgeRising,
                                              (GpioIsrCallback)NULL, NULL));

    TEST_ASSERT_EQUAL_INT(0, fakeTotalCalls(&fakeGpio));

    DeleteGpio(&gpio);
    TEST_ASSERT_NULL(gpio);

    fakeReset(&fakeGpio);
}

/* 用例 8：GpioWrite 的 pin / level 原样透传，且只透传一次 */
static void test_writePassThrough(void)
{
    Gpio* gpio = NULL;

    fakeReset(&fakeGpio);

    gpio = NewGpio(&fakeGpio, fakeInterface());
    TEST_ASSERT_NOT_NULL(gpio);

    TEST_ASSERT_EQUAL_INT(GPIO_OK, GpioWrite(gpio, 7u, GpioLevelHigh));

    TEST_ASSERT_EQUAL_INT(1, fakeGpio.writeCalls);
    TEST_ASSERT_EQUAL_UINT32(7u, fakeGpio.lastPin);
    TEST_ASSERT_EQUAL_INT(GpioLevelHigh, fakeGpio.lastLevel);
    TEST_ASSERT_EQUAL_INT(1, fakeTotalCalls(&fakeGpio));

    DeleteGpio(&gpio);
    TEST_ASSERT_NULL(gpio);

    fakeReset(&fakeGpio);
}

/* 用例 9：GpioRead 把 fake 的电平带出，pin 原样透传 */
static void test_readPassThrough(void)
{
    Gpio*     gpio  = NULL;
    GpioLevel level = GpioLevelLow;

    fakeReset(&fakeGpio);

    fakeGpio.readLevel  = GpioLevelHigh;
    fakeGpio.readResult = GPIO_OK;

    gpio = NewGpio(&fakeGpio, fakeInterface());
    TEST_ASSERT_NOT_NULL(gpio);

    TEST_ASSERT_EQUAL_INT(GPIO_OK, GpioRead(gpio, 7u, &level));

    TEST_ASSERT_EQUAL_INT(1, fakeGpio.readCalls);
    TEST_ASSERT_EQUAL_UINT32(7u, fakeGpio.lastPin);
    TEST_ASSERT_EQUAL_INT(GpioLevelHigh, level);
    TEST_ASSERT_EQUAL_INT(1, fakeTotalCalls(&fakeGpio));

    DeleteGpio(&gpio);
    TEST_ASSERT_NULL(gpio);

    fakeReset(&fakeGpio);
}

/* 用例 10：GpioAttachInterrupt 的 edge / callback / context 原样透传 */
static void test_attachInterruptPassThrough(void)
{
    Gpio* gpio    = NULL;
    int   context = 0;   /* 任意用户上下文，只校验指针原样透传 */

    fakeReset(&fakeGpio);

    gpio = NewGpio(&fakeGpio, fakeInterface());
    TEST_ASSERT_NOT_NULL(gpio);

    TEST_ASSERT_EQUAL_INT(GPIO_OK,
                          GpioAttachInterrupt(gpio, 5u, GpioEdgeFalling, fakeIsrCallback, &context));

    TEST_ASSERT_EQUAL_INT(1, fakeGpio.attachCalls);
    TEST_ASSERT_EQUAL_UINT32(5u, fakeGpio.lastPin);
    TEST_ASSERT_EQUAL_INT(GpioEdgeFalling, fakeGpio.lastEdge);
    TEST_ASSERT_TRUE_MESSAGE(fakeGpio.lastCallback == fakeIsrCallback,
                             "callback 必须原样透传给具体实现");
    TEST_ASSERT_EQUAL_PTR(&context, fakeGpio.lastContext);
    TEST_ASSERT_EQUAL_INT(1, fakeTotalCalls(&fakeGpio));

    /* 透传进来的回调确实可被调用 */
    fakeGpio.lastCallback(fakeGpio.lastContext, 5u);
    TEST_ASSERT_EQUAL_INT(1, isrFireCount);
    TEST_ASSERT_EQUAL_UINT32(5u, isrFirePin);

    DeleteGpio(&gpio);
    TEST_ASSERT_NULL(gpio);

    fakeReset(&fakeGpio);
}

/* 用例 11：虚表项为 NULL 时返回 -2（未实现 / 不支持） */
static void test_unimplementedInterfaceReturnsUnsupported(void)
{
    Gpio* gpio = NULL;

    fakeReset(&fakeGpio);

    gpio = NewGpio(&fakeGpio, fakeInterfaceWithoutInit());
    TEST_ASSERT_NOT_NULL(gpio);

    TEST_ASSERT_EQUAL_INT(GPIO_ERR_UNSUPPORTED, GpioInit(gpio, 1u, GpioDirOutput, GpioPullNone));
    TEST_ASSERT_EQUAL_INT(0, fakeTotalCalls(&fakeGpio));

    DeleteGpio(&gpio);
    TEST_ASSERT_NULL(gpio);

    fakeReset(&fakeGpio);
}

/* 用例 12：分配器配平 —— 分配次数 == 释放次数，用完恢复原钩子 */
static void test_allocatorBalance(void)
{
    void* (*savedMalloc)(size_t) = gpioMalloc;
    void  (*savedFree)(void*)    = gpioFree;
    Gpio*   gpio                 = NULL;

    fakeReset(&fakeGpio);

    allocCount = 0;
    freeCount  = 0;
    gpioMalloc = countingMalloc;
    gpioFree   = countingFree;

    gpio = NewGpio(&fakeGpio, fakeInterface());
    DeleteGpio(&gpio);

    /* 先恢复钩子再断言：Unity 断言失败会 longjmp 跳出本用例，
     * 若放在断言之后恢复，失败路径会污染后续用例的分配器。 */
    gpioMalloc = savedMalloc;
    gpioFree   = savedFree;

    TEST_ASSERT_NULL(gpio);
    TEST_ASSERT_EQUAL_INT(1, allocCount);
    TEST_ASSERT_EQUAL_INT(1, freeCount);
    TEST_ASSERT_EQUAL_INT(allocCount, freeCount);
    TEST_ASSERT_EQUAL_INT(1, fakeGpio.deleteCalls);

    allocCount = 0;
    freeCount  = 0;
    fakeReset(&fakeGpio);
}

/* ==========================================================================
 * 4. 本模块测试入口
 * ========================================================================== */
void RunGpioTests(void)
{
    RUN_TEST(test_newGpioRejectsNullInstance);
    RUN_TEST(test_newGpioRejectsMissingDeleter);
    RUN_TEST(test_newGpioAssemblesInstanceAndInterfaces);
    RUN_TEST(test_deleteGpioCallsDeleterOnce);
    RUN_TEST(test_deleteGpioIsNullSafe);
    RUN_TEST(test_nullHandleIsRejected);
    RUN_TEST(test_nullOutputPointerIsRejected);
    RUN_TEST(test_writePassThrough);
    RUN_TEST(test_readPassThrough);
    RUN_TEST(test_attachInterruptPassThrough);
    RUN_TEST(test_unimplementedInterfaceReturnsUnsupported);
    RUN_TEST(test_allocatorBalance);
}
