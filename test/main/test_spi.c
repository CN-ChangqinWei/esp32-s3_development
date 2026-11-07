/* ============================================================================
 * hal/spi 抽象父类（Spi）单元测试
 *
 * 覆盖范围：只测抽象层 NewSpi / DeleteSpi / SpiXxx 的「入参校验 + 虚表转发」，
 *           具象层用本文件内的 FakeSpi 假冒，不触碰任何真实 SPI 总线、
 *           不引入 ESP-IDF 驱动，因此无需接线即可在目标板上运行。
 *
 * 依赖：仅 <stdint.h> / <stdlib.h> / unity.h / spi.h。
 *
 * 约定：
 *   - 除 RunSpiTests 外，全部 fake 函数与辅助函数均为 static，不污染链接空间；
 *   - 每个用例开始与结束都调用 fakeReset()，用例之间互不影响；
 *   - 用例中替换 spiMalloc / spiFree 时，务必先恢复指针再断言，否则断言失败
 *     会 longjmp 出去，把被替换的钩子泄漏给后续用例；
 *   - 本文件以 hal/spi/src/spi.c 的【实际实现】为准断言，不臆测返回码。
 * ========================================================================== */

#include <stdint.h>
#include <stdlib.h>

#include "unity.h"

#include "spi.h"

/* fake 回填 rx 缓冲区时的上限保护（用例里的缓冲区都很小） */
#define FAKE_SPI_MAX_BYTES  8U

/* ==========================================================================
 * fake 具象：记录调用次数与实参快照，返回值可由用例定制
 * ========================================================================== */
typedef struct FakeSpi {
    /* 调用计数 */
    int initCount;
    int deinitCount;
    int transferCount;
    int writeCount;
    int readCount;
    int selectCount;
    int setConfigCount;
    int deleteCount;

    /* 最近一次调用的实参快照 */
    const uint8_t*   lastTxData;      /* 抽象层是否原样透传了调用者指针 */
    uint8_t*         lastRxData;
    uint32_t         lastLen;
    uint32_t         lastTimeoutMs;
    int              lastActive;      /* 归一化后的片选电平 */
    uint32_t         lastClockHz;
    SpiMode          lastMode;
    uint8_t          lastBitsPerWord;
    const SpiConfig* lastConfigPtr;   /* 指针是否原样透传（无拷贝） */
    void*            deletedInstance; /* deleteInstance 收到的是哪个实例 */

    /* 各虚表槽位的返回值，用例可定制 */
    int retInit;
    int retDeinit;
    int retTransfer;
    int retWrite;
    int retRead;
    int retSelect;
    int retSetConfig;

    /* fakeTransfer 回填 rx 缓冲区的字节 */
    uint8_t readFill;
} FakeSpi;

/* 用例共用一份 fake，避免大结构体落在任务栈上 */
static FakeSpi fake;

/* ==========================================================================
 * fake 状态维护：清零计数、清空快照、恢复默认返回值
 * ========================================================================== */
static void fakeReset(void)
{
    fake.initCount       = 0;
    fake.deinitCount     = 0;
    fake.transferCount   = 0;
    fake.writeCount      = 0;
    fake.readCount       = 0;
    fake.selectCount     = 0;
    fake.setConfigCount  = 0;
    fake.deleteCount     = 0;

    fake.lastTxData      = NULL;
    fake.lastRxData      = NULL;
    fake.lastLen         = 0;
    fake.lastTimeoutMs   = 0;
    fake.lastActive      = -1;   /* -1 表示「未收到过合法归一化值」 */
    fake.lastClockHz     = 0;
    fake.lastMode        = (SpiMode)SpiMode0;
    fake.lastBitsPerWord = 0;
    fake.lastConfigPtr   = NULL;
    fake.deletedInstance = NULL;

    fake.retInit         = 0;
    fake.retDeinit       = 0;
    fake.retTransfer     = 0;
    fake.retWrite        = 0;
    fake.retRead         = 0;
    fake.retSelect       = 0;
    fake.retSetConfig    = 0;

    fake.readFill        = 0x5A;
}

/* ==========================================================================
 * fake 具象的各虚表槽位实现（一律 static，首参 void* instance）
 * 命名统一加 fake 前缀，避开 read / write 等 POSIX 符号
 * ========================================================================== */
static int fakeInit(void* instance)
{
    FakeSpi* self = (FakeSpi*)instance;

    if (NULL == self) {
        return -1;
    }

    self->initCount++;
    return self->retInit;
}

static int fakeDeinit(void* instance)
{
    FakeSpi* self = (FakeSpi*)instance;

    if (NULL == self) {
        return -1;
    }

    self->deinitCount++;
    return self->retDeinit;
}

static int fakeTransfer(void* instance, const uint8_t* txData,
                        uint8_t* rxData, uint32_t len, uint32_t timeoutMs)
{
    FakeSpi* self = (FakeSpi*)instance;
    uint32_t i = 0;

    if (NULL == self) {
        return -1;
    }

    self->transferCount++;
    self->lastTxData    = txData;
    self->lastRxData    = rxData;
    self->lastLen       = len;
    self->lastTimeoutMs = timeoutMs;

    if ((NULL != rxData) && (len <= FAKE_SPI_MAX_BYTES)) {
        for (i = 0; i < len; i++) {
            rxData[i] = self->readFill;
        }
    }

    return self->retTransfer;
}

static int fakeWrite(void* instance, const uint8_t* txData, uint32_t len, uint32_t timeoutMs)
{
    FakeSpi* self = (FakeSpi*)instance;

    if (NULL == self) {
        return -1;
    }

    self->writeCount++;
    self->lastTxData    = txData;
    self->lastLen       = len;
    self->lastTimeoutMs = timeoutMs;

    return self->retWrite;
}

static int fakeRead(void* instance, uint8_t* rxData, uint32_t len, uint32_t timeoutMs)
{
    FakeSpi* self = (FakeSpi*)instance;
    uint32_t i = 0;

    if (NULL == self) {
        return -1;
    }

    self->readCount++;
    self->lastRxData    = rxData;
    self->lastLen       = len;
    self->lastTimeoutMs = timeoutMs;

    if ((NULL != rxData) && (len <= FAKE_SPI_MAX_BYTES)) {
        for (i = 0; i < len; i++) {
            rxData[i] = self->readFill;
        }
    }

    return self->retRead;
}

static int fakeSelect(void* instance, int active)
{
    FakeSpi* self = (FakeSpi*)instance;

    if (NULL == self) {
        return -1;
    }

    self->selectCount++;
    self->lastActive = active;

    return self->retSelect;
}

static int fakeSetConfig(void* instance, const SpiConfig* config)
{
    FakeSpi* self = (FakeSpi*)instance;

    if (NULL == self) {
        return -1;
    }

    self->setConfigCount++;
    self->lastConfigPtr = config;

    if (NULL != config) {
        self->lastClockHz     = config->clockHz;
        self->lastMode        = config->mode;
        self->lastBitsPerWord = config->bitsPerWord;
    }

    return self->retSetConfig;
}

static void fakeDeleteInstance(void* instance)
{
    FakeSpi* self = (FakeSpi*)instance;

    if (NULL == self) {
        return;
    }

    self->deleteCount++;
    self->deletedInstance = instance;
}

/* 聚合初始化构造 fake 虚表（刻意不使用 _RETURN_SPI_INTERFACE* 宏） */
static SpiInterface fakeInterface(void)
{
    SpiInterface interfaces = {
        .init           = fakeInit,
        .deinit         = fakeDeinit,
        .transfer       = fakeTransfer,
        .write          = fakeWrite,
        .read           = fakeRead,
        .select         = fakeSelect,
        .setConfig      = fakeSetConfig,
        .deleteInstance = fakeDeleteInstance
    };

    return interfaces;
}

/* 用共享 fake 装配一个抽象对象（不做 fakeReset，由用例负责） */
static Spi* makeSpi(void)
{
    return NewSpi(&fake, fakeInterface());
}

/* ==========================================================================
 * 用例 1：抽象构造的三条拒绝路径与正常装配
 * ========================================================================== */
static void test_new_spi_validation(void)
{
    SpiInterface noDelete;
    void* (*savedMalloc)(size_t size) = NULL;

    fakeReset();

    /* ① instance 为空 → 拒绝，不分配、不触达具象层 */
    TEST_ASSERT_NULL(NewSpi(NULL, fakeInterface()));

    /* ② deleteInstance 为空 → 拒绝（spi.c 第 12 行强校验） */
    noDelete = fakeInterface();
    noDelete.deleteInstance = NULL;
    TEST_ASSERT_NULL(NewSpi(&fake, noDelete));

    /* ③ 分配器被摘除（spiMalloc == NULL）→ 拒绝，且不调用析构槽位 */
    savedMalloc = spiMalloc;
    spiMalloc   = NULL;
    {
        Spi* none = NewSpi(&fake, fakeInterface());

        spiMalloc = savedMalloc;      /* 先恢复钩子，避免污染后续用例 */
        savedMalloc = NULL;

        TEST_ASSERT_NULL(none);
    }

    /* 以上三条路径都不得触达具象层 */
    TEST_ASSERT_EQUAL_INT(0, fake.deleteCount);
    TEST_ASSERT_EQUAL_INT(0, fake.initCount);
    TEST_ASSERT_EQUAL_INT(0, fake.transferCount);

    /* 正常装配：instance 存外部注入的具象实例，虚表逐字段拷贝 */
    {
        Spi* handle = makeSpi();

        TEST_ASSERT_NOT_NULL(handle);
        TEST_ASSERT_TRUE(&fake == handle->instance);
        TEST_ASSERT_TRUE(fakeInit           == handle->interfaces.init);
        TEST_ASSERT_TRUE(fakeDeinit         == handle->interfaces.deinit);
        TEST_ASSERT_TRUE(fakeTransfer       == handle->interfaces.transfer);
        TEST_ASSERT_TRUE(fakeWrite          == handle->interfaces.write);
        TEST_ASSERT_TRUE(fakeRead           == handle->interfaces.read);
        TEST_ASSERT_TRUE(fakeSelect         == handle->interfaces.select);
        TEST_ASSERT_TRUE(fakeSetConfig      == handle->interfaces.setConfig);
        TEST_ASSERT_TRUE(fakeDeleteInstance == handle->interfaces.deleteInstance);

        DeleteSpi(&handle);
        TEST_ASSERT_NULL(handle);
    }

    fakeReset();
}

/* ==========================================================================
 * 用例 2：析构语义（deleteInstance 恰好一次 + 指针置空 + 空入参安全）
 * ========================================================================== */
static void test_delete_spi_calls_delete_instance_once(void)
{
    Spi* handle = NULL;

    fakeReset();

    handle = makeSpi();
    TEST_ASSERT_NOT_NULL(handle);

    DeleteSpi(&handle);
    TEST_ASSERT_EQUAL_INT(1, fake.deleteCount);          /* 恰好一次 */
    TEST_ASSERT_TRUE(&fake == fake.deletedInstance);     /* 销毁的是注入的实例 */
    TEST_ASSERT_NULL(handle);                            /* 返回后置空 */

    /* 重复析构 / 空入参 / 空指针地址 一律安全，且不再触发 deleteInstance */
    DeleteSpi(&handle);
    DeleteSpi(NULL);
    {
        Spi* empty = NULL;

        DeleteSpi(&empty);
        TEST_ASSERT_NULL(empty);
    }

    TEST_ASSERT_EQUAL_INT(1, fake.deleteCount);
    TEST_ASSERT_EQUAL_INT(0, fake.initCount);

    fakeReset();
}

/* ==========================================================================
 * 用例 3：空句柄与非法参数一律 -1，且不触达具象层
 * ========================================================================== */
static void test_null_handle_and_invalid_args(void)
{
    Spi* handle = NULL;
    SpiConfig badConfig;
    const uint8_t txBuf[4] = { 0x01, 0x02, 0x03, 0x04 };
    uint8_t rxBuf[4] = { 0x00, 0x00, 0x00, 0x00 };

    fakeReset();

    handle = makeSpi();
    TEST_ASSERT_NOT_NULL(handle);

    /* 空句柄 → -1 */
    TEST_ASSERT_EQUAL_INT(-1, SpiInit(NULL));
    TEST_ASSERT_EQUAL_INT(-1, SpiDeinit(NULL));
    TEST_ASSERT_EQUAL_INT(-1, SpiSelect(NULL, 1));
    TEST_ASSERT_EQUAL_INT(-1, SpiSetConfig(NULL, NULL));
    TEST_ASSERT_EQUAL_INT(-1, SpiTransfer(NULL, txBuf, rxBuf, 4, 10));
    TEST_ASSERT_EQUAL_INT(-1, SpiWrite(NULL, txBuf, 4, 10));
    TEST_ASSERT_EQUAL_INT(-1, SpiRead(NULL, rxBuf, 4, 10));

    /* 合法句柄 + 非法参数 → -1 */
    TEST_ASSERT_EQUAL_INT(-1, SpiSetConfig(handle, NULL));   /* config 为空 */

    badConfig.clockHz     = 1000000u;
    badConfig.mode        = SpiMode0;
    badConfig.bitsPerWord = 0;                               /* 位宽为 0 非法 */
    TEST_ASSERT_EQUAL_INT(-1, SpiSetConfig(handle, &badConfig));

    /* 以上调用一律不得触达具象层 */
    TEST_ASSERT_EQUAL_INT(0, fake.initCount);
    TEST_ASSERT_EQUAL_INT(0, fake.deinitCount);
    TEST_ASSERT_EQUAL_INT(0, fake.selectCount);
    TEST_ASSERT_EQUAL_INT(0, fake.setConfigCount);
    TEST_ASSERT_EQUAL_INT(0, fake.transferCount);

    DeleteSpi(&handle);
    TEST_ASSERT_NULL(handle);

    fakeReset();
}

/* ==========================================================================
 * 用例 4：SpiTransfer 边界
 *   - 收发两侧同时为 NULL → -1
 *   - 仅一侧为 NULL → 合法，原样透传
 *   - len == 0 → spi.c 第 71 行直接 return 0（短路，【不转发】给具象层）
 * ========================================================================== */
static void test_transfer_boundary(void)
{
    Spi* handle = NULL;
    const uint8_t txBuf[4] = { 0x11, 0x22, 0x33, 0x44 };
    uint8_t rxBuf[4] = { 0x00, 0x00, 0x00, 0x00 };

    fakeReset();

    handle = makeSpi();
    TEST_ASSERT_NOT_NULL(handle);

    /* 两侧同时为空 → -1，不触达具象层 */
    TEST_ASSERT_EQUAL_INT(-1, SpiTransfer(handle, NULL, NULL, 4, 10));
    TEST_ASSERT_EQUAL_INT(0, fake.transferCount);

    /* 仅 tx（单向写）→ 合法，rxData == NULL 原样透传 */
    fake.retTransfer = 2;
    TEST_ASSERT_EQUAL_INT(2, SpiTransfer(handle, txBuf, NULL, 2, 10));
    TEST_ASSERT_EQUAL_INT(1, fake.transferCount);
    TEST_ASSERT_TRUE(txBuf == fake.lastTxData);
    TEST_ASSERT_NULL(fake.lastRxData);
    TEST_ASSERT_EQUAL_UINT32(2, fake.lastLen);
    TEST_ASSERT_EQUAL_UINT32(10, fake.lastTimeoutMs);

    /* 仅 rx（单向读）→ 合法，txData == NULL 原样透传 */
    fake.retTransfer = 3;
    TEST_ASSERT_EQUAL_INT(3, SpiTransfer(handle, NULL, rxBuf, 3, 15));
    TEST_ASSERT_EQUAL_INT(2, fake.transferCount);
    TEST_ASSERT_NULL(fake.lastTxData);
    TEST_ASSERT_TRUE(rxBuf == fake.lastRxData);
    TEST_ASSERT_EQUAL_UINT32(3, fake.lastLen);
    TEST_ASSERT_EQUAL_UINT32(15, fake.lastTimeoutMs);

    /* len == 0 → 依 spi.c 实现直接返回 0，短路、不转发到具象层 */
    TEST_ASSERT_EQUAL_INT(0, SpiTransfer(handle, txBuf, rxBuf, 0, 10));
    TEST_ASSERT_EQUAL_INT(2, fake.transferCount);        /* 计数未被污染 */

    /* 具象层的负值与正返回值都原样上报 */
    fake.retTransfer = -1;
    TEST_ASSERT_EQUAL_INT(-1, SpiTransfer(handle, txBuf, rxBuf, 4, 10));
    TEST_ASSERT_EQUAL_INT(3, fake.transferCount);

    DeleteSpi(&handle);
    TEST_ASSERT_NULL(handle);

    fakeReset();
}

/* ==========================================================================
 * 用例 5：透传正确性（transfer / select 归一化 / setConfig 值拷贝语义）
 * ========================================================================== */
static void test_transfer_select_config_forwarding(void)
{
    Spi* handle = NULL;
    const uint8_t txData[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
    uint8_t rxData[4] = { 0x00, 0x00, 0x00, 0x00 };
    SpiConfig config;

    fakeReset();

    handle = makeSpi();
    TEST_ASSERT_NOT_NULL(handle);

    /* transfer：指针与数值全部原样透传，rx 数据由具象层回填 */
    fake.retTransfer = 4;
    TEST_ASSERT_EQUAL_INT(4, SpiTransfer(handle, txData, rxData, 4, 20));
    TEST_ASSERT_EQUAL_INT(1, fake.transferCount);
    TEST_ASSERT_TRUE(txData == fake.lastTxData);
    TEST_ASSERT_TRUE(rxData == fake.lastRxData);
    TEST_ASSERT_EQUAL_UINT32(4, fake.lastLen);
    TEST_ASSERT_EQUAL_UINT32(20, fake.lastTimeoutMs);
    TEST_ASSERT_EQUAL_UINT8(0x5A, rxData[0]);            /* fake 回填生效 */
    TEST_ASSERT_EQUAL_UINT8(0x5A, rxData[3]);

    /* write / read：同样原样透传，返回值来自具象层 */
    fake.retWrite = 4;
    TEST_ASSERT_EQUAL_INT(4, SpiWrite(handle, txData, 4, 30));
    TEST_ASSERT_EQUAL_INT(1, fake.writeCount);
    TEST_ASSERT_TRUE(txData == fake.lastTxData);
    TEST_ASSERT_EQUAL_UINT32(4, fake.lastLen);
    TEST_ASSERT_EQUAL_UINT32(30, fake.lastTimeoutMs);

    fake.retRead = 4;
    TEST_ASSERT_EQUAL_INT(4, SpiRead(handle, rxData, 4, 35));
    TEST_ASSERT_EQUAL_INT(1, fake.readCount);
    TEST_ASSERT_TRUE(rxData == fake.lastRxData);
    TEST_ASSERT_EQUAL_UINT32(35, fake.lastTimeoutMs);

    /* select：抽象层把非 0 归一化为 1、0 归一化为 0 再透传 */
    TEST_ASSERT_EQUAL_INT(0, SpiSelect(handle, 1));
    TEST_ASSERT_EQUAL_INT(1, fake.selectCount);
    TEST_ASSERT_EQUAL_INT(1, fake.lastActive);

    TEST_ASSERT_EQUAL_INT(0, SpiSelect(handle, 0));
    TEST_ASSERT_EQUAL_INT(2, fake.selectCount);
    TEST_ASSERT_EQUAL_INT(0, fake.lastActive);

    TEST_ASSERT_EQUAL_INT(0, SpiSelect(handle, 42));     /* 任意非 0 → 1 */
    TEST_ASSERT_EQUAL_INT(3, fake.selectCount);
    TEST_ASSERT_EQUAL_INT(1, fake.lastActive);

    TEST_ASSERT_EQUAL_INT(0, SpiSelect(handle, -7));     /* 负非 0 → 1 */
    TEST_ASSERT_EQUAL_INT(4, fake.selectCount);
    TEST_ASSERT_EQUAL_INT(1, fake.lastActive);

    /* setConfig：clockHz / mode / bitsPerWord 原样透传，指针不做拷贝 */
    config.clockHz     = 8000000u;
    config.mode        = SpiMode3;
    config.bitsPerWord = 8;
    TEST_ASSERT_EQUAL_INT(0, SpiSetConfig(handle, &config));
    TEST_ASSERT_EQUAL_INT(1, fake.setConfigCount);
    TEST_ASSERT_TRUE(&config == fake.lastConfigPtr);
    TEST_ASSERT_EQUAL_UINT32(8000000u, fake.lastClockHz);
    TEST_ASSERT_EQUAL_INT((int)SpiMode3, (int)fake.lastMode);
    TEST_ASSERT_EQUAL_UINT8(8, fake.lastBitsPerWord);

    /* 具象层负值原样上报 */
    fake.retSetConfig = -2;
    config.clockHz     = 1000000u;
    config.mode        = SpiMode1;
    config.bitsPerWord = 16;
    TEST_ASSERT_EQUAL_INT(-2, SpiSetConfig(handle, &config));
    TEST_ASSERT_EQUAL_INT(2, fake.setConfigCount);
    TEST_ASSERT_EQUAL_UINT32(1000000u, fake.lastClockHz);
    TEST_ASSERT_EQUAL_INT((int)SpiMode1, (int)fake.lastMode);
    TEST_ASSERT_EQUAL_UINT8(16, fake.lastBitsPerWord);

    DeleteSpi(&handle);
    TEST_ASSERT_NULL(handle);

    fakeReset();
}

/* ==========================================================================
 * 用例 6：生命周期转发 + 虚表槽位缺失（→ -2，未实现）
 * ========================================================================== */
static void test_lifecycle_and_missing_slots(void)
{
    Spi* handle = NULL;
    SpiInterface partial;

    fakeReset();

    handle = makeSpi();
    TEST_ASSERT_NOT_NULL(handle);

    TEST_ASSERT_EQUAL_INT(0, SpiInit(handle));
    TEST_ASSERT_EQUAL_INT(1, fake.initCount);

    TEST_ASSERT_EQUAL_INT(0, SpiDeinit(handle));
    TEST_ASSERT_EQUAL_INT(1, fake.deinitCount);

    /* 具象层返回非 0 时原样上报 */
    fake.retInit = -3;
    TEST_ASSERT_EQUAL_INT(-3, SpiInit(handle));
    TEST_ASSERT_EQUAL_INT(2, fake.initCount);

    fake.retDeinit = -4;
    TEST_ASSERT_EQUAL_INT(-4, SpiDeinit(handle));
    TEST_ASSERT_EQUAL_INT(2, fake.deinitCount);

    DeleteSpi(&handle);
    TEST_ASSERT_NULL(handle);

    /* 槽位缺失（NULL）→ -2（未实现），不得崩溃、不得误调其它槽位 */
    fakeReset();
    partial = fakeInterface();
    partial.init      = NULL;
    partial.deinit    = NULL;
    partial.transfer  = NULL;
    partial.write     = NULL;
    partial.read      = NULL;
    partial.select    = NULL;
    partial.setConfig = NULL;
    {
        Spi* sparse = NewSpi(&fake, partial);
        const uint8_t txBuf[2] = { 0x01, 0x02 };
        uint8_t rxBuf[2] = { 0x00, 0x00 };
        SpiConfig config;

        TEST_ASSERT_NOT_NULL(sparse);

        TEST_ASSERT_EQUAL_INT(-2, SpiInit(sparse));
        TEST_ASSERT_EQUAL_INT(-2, SpiDeinit(sparse));
        TEST_ASSERT_EQUAL_INT(-2, SpiTransfer(sparse, txBuf, rxBuf, 2, 10));
        TEST_ASSERT_EQUAL_INT(-2, SpiWrite(sparse, txBuf, 2, 10));
        TEST_ASSERT_EQUAL_INT(-2, SpiRead(sparse, rxBuf, 2, 10));
        TEST_ASSERT_EQUAL_INT(-2, SpiSelect(sparse, 1));

        config.clockHz     = 1000000u;
        config.mode        = SpiMode0;
        config.bitsPerWord = 8;
        TEST_ASSERT_EQUAL_INT(-2, SpiSetConfig(sparse, &config));

        TEST_ASSERT_EQUAL_INT(0, fake.initCount);
        TEST_ASSERT_EQUAL_INT(0, fake.deinitCount);
        TEST_ASSERT_EQUAL_INT(0, fake.transferCount);
        TEST_ASSERT_EQUAL_INT(0, fake.writeCount);
        TEST_ASSERT_EQUAL_INT(0, fake.readCount);
        TEST_ASSERT_EQUAL_INT(0, fake.selectCount);
        TEST_ASSERT_EQUAL_INT(0, fake.setConfigCount);

        DeleteSpi(&sparse);               /* 析构槽位仍在，恰好调用一次 */
        TEST_ASSERT_NULL(sparse);
        TEST_ASSERT_EQUAL_INT(1, fake.deleteCount);
    }

    fakeReset();
}

/* ==========================================================================
 * 用例 7：SpiDefaultConfig 默认值（mode0 / 8bit，频率原样带入）
 * ========================================================================== */
static void test_default_config(void)
{
    SpiConfig config;

    fakeReset();

    config = SpiDefaultConfig(20000000u);
    TEST_ASSERT_EQUAL_UINT32(20000000u, config.clockHz);
    TEST_ASSERT_EQUAL_INT((int)SpiMode0, (int)config.mode);
    TEST_ASSERT_EQUAL_UINT8(8, config.bitsPerWord);

    /* 频率为 0 时也照抄，抽象层不校验频率合法性 */
    config = SpiDefaultConfig(0u);
    TEST_ASSERT_EQUAL_UINT32(0u, config.clockHz);
    TEST_ASSERT_EQUAL_INT((int)SpiMode0, (int)config.mode);
    TEST_ASSERT_EQUAL_UINT8(8, config.bitsPerWord);

    TEST_ASSERT_EQUAL_INT(0, fake.transferCount);

    fakeReset();
}

/* ==========================================================================
 * 用例 8：分配器配平（NewSpi + DeleteSpi 后 分配次数 == 释放次数）
 * ========================================================================== */
static void* (*savedSpiMalloc)(size_t size) = NULL;
static void  (*savedSpiFree)(void* p)       = NULL;
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
    Spi* handle   = NULL;
    int  allocs   = 0;
    int  releases = 0;
    int  built    = 0;

    fakeReset();

    /* 替换内存钩子为计数版（内部仍 malloc / free） */
    savedSpiMalloc = spiMalloc;
    savedSpiFree   = spiFree;
    allocTimes     = 0;
    releaseTimes   = 0;
    spiMalloc      = countingMalloc;
    spiFree        = countingFree;

    handle = makeSpi();

    built  = (NULL != handle);
    allocs = allocTimes;

    DeleteSpi(&handle);
    releases = releaseTimes;

    /* 先恢复原始钩子再做断言：断言失败会 longjmp，避免污染后续用例 */
    spiMalloc      = savedSpiMalloc;
    spiFree        = savedSpiFree;
    savedSpiMalloc = NULL;
    savedSpiFree   = NULL;

    TEST_ASSERT_TRUE(built);
    TEST_ASSERT_NULL(handle);                 /* 析构后置空 */
    TEST_ASSERT_EQUAL_INT(1, allocs);         /* NewSpi 只分配 1 次 */
    TEST_ASSERT_EQUAL_INT(allocs, releases);  /* 分配与释放配平 */
    TEST_ASSERT_EQUAL_INT(1, fake.deleteCount);

    fakeReset();
}

/* ==========================================================================
 * 用例汇总入口（供 test_main.c 调用）
 * ========================================================================== */
void RunSpiTests(void)
{
    RUN_TEST(test_new_spi_validation);
    RUN_TEST(test_delete_spi_calls_delete_instance_once);
    RUN_TEST(test_null_handle_and_invalid_args);
    RUN_TEST(test_transfer_boundary);
    RUN_TEST(test_transfer_select_config_forwarding);
    RUN_TEST(test_lifecycle_and_missing_slots);
    RUN_TEST(test_default_config);
    RUN_TEST(test_allocator_balance);
}
