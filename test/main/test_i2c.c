/* ============================================================================
 * hal/i2c 抽象父类（I2c）单元测试
 *
 * 覆盖范围：只测抽象层 NewI2c / DeleteI2c / I2cXxx 的「入参校验 + 虚表转发」，
 *           具象层用本文件内的 FakeI2c 假冒，不发任何真实 I2C 总线访问，
 *           因此不需要接线即可运行。
 *
 * 依赖：仅 <stdint.h> / <stdlib.h> / unity.h / i2c.h，不引入 ESP-IDF 头。
 *
 * 约定：
 *   - 全部 fake 函数与辅助函数均为 static，不污染其它用例文件的链接空间；
 *   - 每个用例开始与结束都清零 fake 计数，用例之间互不影响；
 *   - 用例中替换 i2cMalloc / i2cFree 时，务必先恢复指针再断言，
 *     否则断言失败会 longjmp 出去，把被替换的钩子泄漏给后续用例。
 * ========================================================================== */

#include <stdint.h>
#include <stdlib.h>

#include "unity.h"

#include "i2c.h"

/* fake 单次快照的最大字节数 */
#define FAKE_I2C_MAX_BYTES  16U

/* ==========================================================================
 * fake 具象：记录调用次数与实参快照，返回值可由用例定制
 * ========================================================================== */
typedef struct FakeI2c {
    /* 调用计数 */
    int initCount;
    int deinitCount;
    int writeCount;
    int readCount;
    int writeReadCount;
    int probeCount;
    int setClockCount;
    int resetCount;
    int deleteCount;

    /* 最近一次调用的实参快照 */
    uint16_t lastAddress;
    uint32_t lastTimeoutMs;
    uint32_t lastWriteLen;
    uint32_t lastReadLen;
    uint32_t lastClockHz;
    const uint8_t* lastWritePtr;      /* 抽象层是否原样透传了调用者指针 */
    uint8_t  capturedWrite[FAKE_I2C_MAX_BYTES];
    uint32_t capturedWriteLen;
    void*    deletedInstance;         /* deleteInstance 收到的是哪个实例 */

    /* 各虚表槽位的返回值，用例可定制 */
    int retInit;
    int retDeinit;
    int retWrite;
    int retRead;
    int retWriteRead;
    int retProbe;                     /* 0 表示设备在线 */
    int retSetClock;
    int retReset;

    /* read / writeRead 填回缓冲区的字节 */
    uint8_t readFill;
} FakeI2c;

/* 用例共用一份 fake，避免大结构体落在任务栈上 */
static FakeI2c fake;

/* --------------------------------------------------------------------------
 * fake 状态维护
 * -------------------------------------------------------------------------- */
static void fakeZeroSnapshot(FakeI2c* self)
{
    uint32_t i = 0;

    self->lastAddress      = 0;
    self->lastTimeoutMs    = 0;
    self->lastWriteLen     = 0;
    self->lastReadLen      = 0;
    self->lastClockHz      = 0;
    self->lastWritePtr     = NULL;
    self->capturedWriteLen = 0;
    self->deletedInstance  = NULL;

    for (i = 0; i < FAKE_I2C_MAX_BYTES; i++) {
        self->capturedWrite[i] = 0;
    }
}

/* 清零全部计数与实参快照（不动用例定制的返回值） */
static void fakeResetCounters(FakeI2c* self)
{
    if (NULL == self) {
        return;
    }

    self->initCount     = 0;
    self->deinitCount   = 0;
    self->writeCount    = 0;
    self->readCount     = 0;
    self->writeReadCount = 0;
    self->probeCount    = 0;
    self->setClockCount = 0;
    self->resetCount    = 0;
    self->deleteCount   = 0;

    fakeZeroSnapshot(self);
}

/* 清零计数并恢复默认返回值，供每个用例的起点调用 */
static void fakePrepare(FakeI2c* self)
{
    if (NULL == self) {
        return;
    }

    fakeResetCounters(self);

    self->retInit       = 0;
    self->retDeinit     = 0;
    self->retWrite      = 0;
    self->retRead       = 0;
    self->retWriteRead  = 0;
    self->retProbe      = 0;
    self->retSetClock   = 0;
    self->retReset      = 0;

    self->readFill      = 0xA5;
}

/* ==========================================================================
 * fake 具象的各虚表槽位实现（一律 static，首参 void* instance）
 * ========================================================================== */
static int fakeInit(void* instance)
{
    FakeI2c* self = (FakeI2c*)instance;

    if (NULL == self) {
        return -1;
    }

    self->initCount++;
    return self->retInit;
}

static int fakeDeinit(void* instance)
{
    FakeI2c* self = (FakeI2c*)instance;

    if (NULL == self) {
        return -1;
    }

    self->deinitCount++;
    return self->retDeinit;
}

static int fakeWrite(void* instance, uint16_t deviceAddress,
                     const uint8_t* data, uint32_t len, uint32_t timeoutMs)
{
    FakeI2c* self = (FakeI2c*)instance;
    uint32_t i = 0;

    if (NULL == self) {
        return -1;
    }

    self->writeCount++;
    self->lastAddress      = deviceAddress;
    self->lastTimeoutMs    = timeoutMs;
    self->lastWriteLen     = len;
    self->lastWritePtr     = data;
    self->capturedWriteLen = 0;

    if ((NULL != data) && (len <= FAKE_I2C_MAX_BYTES)) {
        for (i = 0; i < len; i++) {
            self->capturedWrite[i] = data[i];
        }
        self->capturedWriteLen = len;
    }

    return self->retWrite;
}

static int fakeRead(void* instance, uint16_t deviceAddress,
                    uint8_t* buffer, uint32_t len, uint32_t timeoutMs)
{
    FakeI2c* self = (FakeI2c*)instance;
    uint32_t i = 0;

    if (NULL == self) {
        return -1;
    }

    self->readCount++;
    self->lastAddress   = deviceAddress;
    self->lastTimeoutMs = timeoutMs;
    self->lastReadLen   = len;

    if (NULL != buffer) {
        for (i = 0; i < len; i++) {
            buffer[i] = self->readFill;
        }
    }

    return self->retRead;
}

static int fakeWriteRead(void* instance, uint16_t deviceAddress,
                         const uint8_t* writeData, uint32_t writeLen,
                         uint8_t* readBuffer, uint32_t readLen, uint32_t timeoutMs)
{
    FakeI2c* self = (FakeI2c*)instance;
    uint32_t i = 0;

    if (NULL == self) {
        return -1;
    }

    self->writeReadCount++;
    self->lastAddress      = deviceAddress;
    self->lastTimeoutMs    = timeoutMs;
    self->lastWriteLen     = writeLen;
    self->lastReadLen      = readLen;
    self->lastWritePtr     = writeData;
    self->capturedWriteLen = 0;

    if ((NULL != writeData) && (writeLen <= FAKE_I2C_MAX_BYTES)) {
        for (i = 0; i < writeLen; i++) {
            self->capturedWrite[i] = writeData[i];
        }
        self->capturedWriteLen = writeLen;
    }

    if (NULL != readBuffer) {
        for (i = 0; i < readLen; i++) {
            readBuffer[i] = self->readFill;
        }
    }

    return self->retWriteRead;
}

static int fakeProbe(void* instance, uint16_t deviceAddress)
{
    FakeI2c* self = (FakeI2c*)instance;

    if (NULL == self) {
        return -1;
    }

    self->probeCount++;
    self->lastAddress = deviceAddress;

    return self->retProbe;
}

static int fakeSetClock(void* instance, uint32_t clockHz)
{
    FakeI2c* self = (FakeI2c*)instance;

    if (NULL == self) {
        return -1;
    }

    self->setClockCount++;
    self->lastClockHz = clockHz;

    return self->retSetClock;
}

static int fakeReset(void* instance)
{
    FakeI2c* self = (FakeI2c*)instance;

    if (NULL == self) {
        return -1;
    }

    self->resetCount++;
    return self->retReset;
}

static void fakeDeleteInstance(void* instance)
{
    FakeI2c* self = (FakeI2c*)instance;

    if (NULL == self) {
        return;
    }

    self->deleteCount++;
    self->deletedInstance = instance;
}

/* 聚合初始化构造 fake 虚表（刻意不使用 _RETURN_I2C_INTERFACE 宏） */
static I2cInterface fakeInterface(void)
{
    I2cInterface interfaces = {
        .init           = fakeInit,
        .deinit         = fakeDeinit,
        .write          = fakeWrite,
        .read           = fakeRead,
        .writeRead      = fakeWriteRead,
        .probe          = fakeProbe,
        .setClock       = fakeSetClock,
        .reset          = fakeReset,
        .deleteInstance = fakeDeleteInstance
    };

    return interfaces;
}

/* 每个用例的统一入口：复位 fake + 装配抽象对象 */
static I2c* makeI2c(FakeI2c* self)
{
    if (NULL == self) {
        return NULL;
    }

    fakePrepare(self);

    return NewI2c(self, fakeInterface());
}

/* ==========================================================================
 * 用例 1：抽象构造的入参校验与装配结果
 * ========================================================================== */
static void test_new_i2c_validation(void)
{
    I2cInterface noDelete;

    fakePrepare(&fake);

    /* instance 为空 → 直接拒绝，不分配、不触达具象层 */
    TEST_ASSERT_NULL(NewI2c(NULL, fakeInterface()));

    /* 正常装配：instance 存的是外部注入的具象实例，虚表逐字段拷贝 */
    {
        I2c* handle = NewI2c(&fake, fakeInterface());

        TEST_ASSERT_NOT_NULL(handle);
        TEST_ASSERT_TRUE(&fake == handle->instance);
        TEST_ASSERT_TRUE(fakeWrite == handle->interfaces.write);
        TEST_ASSERT_TRUE(fakeWriteRead == handle->interfaces.writeRead);
        TEST_ASSERT_TRUE(fakeDeleteInstance == handle->interfaces.deleteInstance);

        DeleteI2c(&handle);
        TEST_ASSERT_NULL(handle);
    }

    /* deleteInstance 为空：抽象层强校验，直接拒绝（与 gpio/uart/spi/pwm/adc 一致），
     * 避免 DeleteI2c 无法销毁具体实例而泄漏。 */
    fakePrepare(&fake);
    noDelete = fakeInterface();
    noDelete.deleteInstance = NULL;
    TEST_ASSERT_NULL(NewI2c(&fake, noDelete));
    TEST_ASSERT_EQUAL_INT(0, fake.deleteCount);

    fakeResetCounters(&fake);
}

/* ==========================================================================
 * 用例 2：析构语义（deleteInstance 恰好一次 + 指针置空 + 空入参安全）
 * ========================================================================== */
static void test_delete_i2c_calls_delete_instance_once(void)
{
    I2c* handle = makeI2c(&fake);

    TEST_ASSERT_NOT_NULL(handle);

    DeleteI2c(&handle);
    TEST_ASSERT_EQUAL_INT(1, fake.deleteCount);          /* 恰好一次 */
    TEST_ASSERT_TRUE(&fake == fake.deletedInstance);     /* 销毁的是注入的实例 */
    TEST_ASSERT_NULL(handle);                            /* 返回后置空 */

    /* 重复析构 / 空入参一律安全，且不再触发 deleteInstance */
    DeleteI2c(&handle);
    DeleteI2c(NULL);
    {
        I2c* empty = NULL;

        DeleteI2c(&empty);
        TEST_ASSERT_NULL(empty);
    }

    TEST_ASSERT_EQUAL_INT(1, fake.deleteCount);

    fakeResetCounters(&fake);
}

/* ==========================================================================
 * 用例 3：空句柄与非法参数一律 -1，且不触达具象层
 * ========================================================================== */
static void test_null_handle_and_invalid_args(void)
{
    I2c* handle = makeI2c(&fake);
    const uint8_t data[2] = { 0x01, 0x02 };
    uint8_t buffer[2] = { 0x00, 0x00 };

    TEST_ASSERT_NOT_NULL(handle);

    /* 空句柄 */
    TEST_ASSERT_EQUAL_INT(-1, I2cInit(NULL));
    TEST_ASSERT_EQUAL_INT(-1, I2cDeinit(NULL));
    TEST_ASSERT_EQUAL_INT(-1, I2cReset(NULL));
    TEST_ASSERT_EQUAL_INT(-1, I2cSetClock(NULL, 100000));
    TEST_ASSERT_EQUAL_INT(-1, I2cWrite(NULL, 0x50, data, 2, 10));
    TEST_ASSERT_EQUAL_INT(-1, I2cRead(NULL, 0x50, buffer, 2, 10));
    TEST_ASSERT_EQUAL_INT(-1, I2cProbe(NULL, 0x50));

    /* 合法句柄 + 非法数据参数 */
    TEST_ASSERT_EQUAL_INT(-1, I2cWrite(handle, 0x50, NULL, 2, 10));   /* data 为空 */
    TEST_ASSERT_EQUAL_INT(-1, I2cWrite(handle, 0x50, data, 0, 10));   /* len 为 0 */
    TEST_ASSERT_EQUAL_INT(-1, I2cRead(handle, 0x50, NULL, 2, 10));    /* buffer 为空 */
    TEST_ASSERT_EQUAL_INT(-1, I2cRead(handle, 0x50, buffer, 0, 10));  /* len 为 0 */
    TEST_ASSERT_EQUAL_INT(-1, I2cSetClock(handle, 0));                /* 时钟为 0 */

    /* 以上调用一律不得触达具象层 */
    TEST_ASSERT_EQUAL_INT(0, fake.initCount);
    TEST_ASSERT_EQUAL_INT(0, fake.deinitCount);
    TEST_ASSERT_EQUAL_INT(0, fake.writeCount);
    TEST_ASSERT_EQUAL_INT(0, fake.readCount);
    TEST_ASSERT_EQUAL_INT(0, fake.writeReadCount);
    TEST_ASSERT_EQUAL_INT(0, fake.probeCount);
    TEST_ASSERT_EQUAL_INT(0, fake.setClockCount);
    TEST_ASSERT_EQUAL_INT(0, fake.resetCount);

    DeleteI2c(&handle);
    fakeResetCounters(&fake);
}

/* ==========================================================================
 * 用例 4：I2cWriteRead 的边界判定（两侧皆 0 非法，单侧为 0 合法透传）
 * ========================================================================== */
static void test_write_read_boundary(void)
{
    I2c* handle = makeI2c(&fake);
    const uint8_t writeData[2] = { 0xA0, 0xA1 };
    uint8_t readBuffer[3] = { 0x00, 0x00, 0x00 };

    TEST_ASSERT_NOT_NULL(handle);

    /* 两侧长度都为 0 → 非法，不触达具象层 */
    TEST_ASSERT_EQUAL_INT(-1, I2cWriteRead(handle, 0x50, NULL, 0, NULL, 0, 10));
    TEST_ASSERT_EQUAL_INT(0, fake.writeReadCount);

    /* 仅读（writeLen == 0）→ 合法，allow NULL writeData，原样透传 */
    fake.retWriteRead = 0;
    TEST_ASSERT_EQUAL_INT(0, I2cWriteRead(handle, 0x50, NULL, 0, readBuffer, 3, 20));
    TEST_ASSERT_EQUAL_INT(1, fake.writeReadCount);
    TEST_ASSERT_EQUAL_UINT16(0x50, fake.lastAddress);
    TEST_ASSERT_EQUAL_UINT32(0, fake.lastWriteLen);
    TEST_ASSERT_EQUAL_UINT32(3, fake.lastReadLen);
    TEST_ASSERT_EQUAL_UINT32(20, fake.lastTimeoutMs);

    /* 仅写（readLen == 0）→ 合法，allow NULL readBuffer，原样透传 */
    TEST_ASSERT_EQUAL_INT(0, I2cWriteRead(handle, 0x50, writeData, 2, NULL, 0, 25));
    TEST_ASSERT_EQUAL_INT(2, fake.writeReadCount);
    TEST_ASSERT_EQUAL_UINT32(2, fake.lastWriteLen);
    TEST_ASSERT_EQUAL_UINT32(0, fake.lastReadLen);
    TEST_ASSERT_EQUAL_UINT32(25, fake.lastTimeoutMs);
    TEST_ASSERT_EQUAL_UINT8(writeData[0], fake.capturedWrite[0]);
    TEST_ASSERT_EQUAL_UINT8(writeData[1], fake.capturedWrite[1]);

    /* 长度非 0 但对应缓冲区为空 → 非法 */
    TEST_ASSERT_EQUAL_INT(-1, I2cWriteRead(handle, 0x50, NULL, 2, readBuffer, 3, 10));
    TEST_ASSERT_EQUAL_INT(-1, I2cWriteRead(handle, 0x50, writeData, 2, NULL, 3, 10));
    TEST_ASSERT_EQUAL_INT(2, fake.writeReadCount);   /* 计数未被非法调用污染 */

    DeleteI2c(&handle);
    fakeResetCounters(&fake);
}

/* ==========================================================================
 * 用例 5：写 / 探测的透传正确性（地址、长度、超时、数据、返回值）
 * ========================================================================== */
static void test_write_and_probe_forwarding(void)
{
    I2c* handle = makeI2c(&fake);
    const uint8_t data[3] = { 0x3C, 0x5A, 0xFF };

    TEST_ASSERT_NOT_NULL(handle);

    fake.retWrite = 0;
    TEST_ASSERT_EQUAL_INT(0, I2cWrite(handle, 0x3C, data, 2, 15));
    TEST_ASSERT_EQUAL_INT(1, fake.writeCount);
    TEST_ASSERT_EQUAL_UINT16(0x3C, fake.lastAddress);
    TEST_ASSERT_EQUAL_UINT32(2, fake.lastWriteLen);
    TEST_ASSERT_EQUAL_UINT32(15, fake.lastTimeoutMs);
    TEST_ASSERT_TRUE(data == fake.lastWritePtr);        /* 指针原样透传，无拷贝 */
    TEST_ASSERT_EQUAL_UINT32(2, fake.capturedWriteLen);
    TEST_ASSERT_EQUAL_UINT8(data[0], fake.capturedWrite[0]);   /* 逐字节一致 */
    TEST_ASSERT_EQUAL_UINT8(data[1], fake.capturedWrite[1]);

    /* probe：地址透传，返回值原样上报（0 表示在线） */
    fake.retProbe = 0;
    TEST_ASSERT_EQUAL_INT(0, I2cProbe(handle, 0x77));
    TEST_ASSERT_EQUAL_INT(1, fake.probeCount);
    TEST_ASSERT_EQUAL_UINT16(0x77, fake.lastAddress);

    /* probe：具象层的负值（无应答）也必须原样上报 */
    fake.retProbe = -3;
    TEST_ASSERT_EQUAL_INT(-3, I2cProbe(handle, 0x77));
    TEST_ASSERT_EQUAL_INT(2, fake.probeCount);

    DeleteI2c(&handle);
    fakeResetCounters(&fake);
}

/* ==========================================================================
 * 用例 6：生命周期与配置透传（含虚表槽位缺失 → -2）
 * ========================================================================== */
static void test_lifecycle_and_clock_forwarding(void)
{
    I2c* handle = makeI2c(&fake);
    I2cInterface partial;

    TEST_ASSERT_NOT_NULL(handle);

    TEST_ASSERT_EQUAL_INT(0, I2cInit(handle));
    TEST_ASSERT_EQUAL_INT(1, fake.initCount);

    TEST_ASSERT_EQUAL_INT(0, I2cDeinit(handle));
    TEST_ASSERT_EQUAL_INT(1, fake.deinitCount);

    TEST_ASSERT_EQUAL_INT(0, I2cReset(handle));
    TEST_ASSERT_EQUAL_INT(1, fake.resetCount);

    TEST_ASSERT_EQUAL_INT(0, I2cSetClock(handle, 400000));
    TEST_ASSERT_EQUAL_INT(1, fake.setClockCount);
    TEST_ASSERT_EQUAL_UINT32(400000, fake.lastClockHz);

    /* 具象层返回非 0 时原样上报 */
    fake.retInit = -3;
    TEST_ASSERT_EQUAL_INT(-3, I2cInit(handle));
    TEST_ASSERT_EQUAL_INT(2, fake.initCount);

    DeleteI2c(&handle);

    /* 虚表槽位缺失（NULL）→ -2（未实现），不得崩溃 */
    fakePrepare(&fake);
    partial = fakeInterface();
    partial.init     = NULL;
    partial.deinit   = NULL;
    partial.reset    = NULL;
    partial.setClock = NULL;
    {
        I2c* sparse = NewI2c(&fake, partial);

        TEST_ASSERT_NOT_NULL(sparse);

        TEST_ASSERT_EQUAL_INT(-2, I2cInit(sparse));
        TEST_ASSERT_EQUAL_INT(-2, I2cDeinit(sparse));
        TEST_ASSERT_EQUAL_INT(-2, I2cReset(sparse));
        TEST_ASSERT_EQUAL_INT(-2, I2cSetClock(sparse, 100000));

        TEST_ASSERT_EQUAL_INT(0, fake.initCount);
        TEST_ASSERT_EQUAL_INT(0, fake.deinitCount);
        TEST_ASSERT_EQUAL_INT(0, fake.resetCount);
        TEST_ASSERT_EQUAL_INT(0, fake.setClockCount);

        DeleteI2c(&sparse);
        TEST_ASSERT_NULL(sparse);
    }

    fakeResetCounters(&fake);
}

/* ==========================================================================
 * 用例 7：分配器配平（NewI2c + DeleteI2c 后 分配次数 == 释放次数）
 * ========================================================================== */
static void* (*savedI2cMalloc)(size_t size) = NULL;
static void  (*savedI2cFree)(void* p)       = NULL;
static int    allocTimes;
static int    releaseTimes;

static void* countingMalloc(size_t size)
{
    void* p = malloc(size);

    if (NULL != p) {
        allocTimes++;
    }

    return p;
}

static void countingFree(void* p)
{
    if (NULL != p) {
        releaseTimes++;
    }

    free(p);
}

static void test_allocator_balance(void)
{
    I2c* handle = NULL;
    int allocs    = 0;
    int releases  = 0;
    int built     = 0;

    /* 替换内存钩子为计数版（内部仍 malloc / free） */
    savedI2cMalloc = i2cMalloc;
    savedI2cFree   = i2cFree;
    allocTimes     = 0;
    releaseTimes   = 0;
    i2cMalloc      = countingMalloc;
    i2cFree        = countingFree;

    fakePrepare(&fake);
    handle = NewI2c(&fake, fakeInterface());

    built  = (NULL != handle);
    allocs = allocTimes;

    DeleteI2c(&handle);
    releases = releaseTimes;

    /* 先恢复原始钩子再做断言：断言失败会 longjmp，避免污染后续用例 */
    i2cMalloc      = savedI2cMalloc;
    i2cFree        = savedI2cFree;
    savedI2cMalloc = NULL;
    savedI2cFree   = NULL;

    TEST_ASSERT_TRUE(built);
    TEST_ASSERT_NULL(handle);                 /* 析构后置空 */
    TEST_ASSERT_EQUAL_INT(1, allocs);         /* NewI2c 只分配 1 次 */
    TEST_ASSERT_EQUAL_INT(allocs, releases);  /* 分配与释放配平 */
    TEST_ASSERT_EQUAL_INT(1, fake.deleteCount);

    fakeResetCounters(&fake);
}

/* ==========================================================================
 * 用例汇总入口（供 test_main.c 调用）
 * ========================================================================== */
void RunI2cTests(void)
{
    RUN_TEST(test_new_i2c_validation);
    RUN_TEST(test_delete_i2c_calls_delete_instance_once);
    RUN_TEST(test_null_handle_and_invalid_args);
    RUN_TEST(test_write_read_boundary);
    RUN_TEST(test_write_and_probe_forwarding);
    RUN_TEST(test_lifecycle_and_clock_forwarding);
    RUN_TEST(test_allocator_balance);
}
