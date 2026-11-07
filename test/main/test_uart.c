/* ============================================================================
 * hal/uart —— 抽象父类单元测试（fake 具象，不访问任何真实串口硬件）
 *
 * 覆盖点：
 *   ① UartDefaultConfig 默认值（8N1、无流控、缓冲区走平台默认）；
 *   ② NewUart 的非法入参拒绝 + 正常装配（instance / interfaces 正确落位）；
 *   ③ DeleteUart 的语义（deleteInstance 恰好一次、外部指针置空、NULL 安全）；
 *   ④ 空句柄 / 非法入参安全（全部返回 -1，且不触碰具象实现）；
 *   ⑤ 透传正确性（write / read / setBaudRate / init / deinit / flush /
 *      bytesAvailable 的参数与返回值与具象实现逐项一致）；
 *   ⑥ 虚表缺项返回 -2；
 *   ⑦ 分配器配平（替换 uartMalloc/uartFree 后 New+Delete 次数相等，并还原）。
 *
 * 说明：
 *   - fake 实例 gFake 为静态对象，fakeDeleteInstance 只做计数与标记，
 *     绝不 free，避免释放非堆内存；
 *   - 每个用例开头与结尾都调用 fakeReset()，用例之间互不影响；
 *   - 本文件只依赖 C 标准头 + unity + 抽象头 uart.h，不引入 ESP-IDF 头。
 * ========================================================================== */

#include <stdint.h>
#include <stdlib.h>

#include "unity.h"

#include "uart.h"

/* ============================================================================
 * fake 具象：实例 + 调用记录 + 预设返回值
 * ========================================================================== */
typedef struct FakeUart {
    /* ---------- 调用计数 ---------- */
    int initCount;
    int deinitCount;
    int writeCount;
    int readCount;
    int flushCount;
    int bytesAvailableCount;
    int setBaudRateCount;
    int deleteCount;

    /* ---------- 最近一次调用的入参记录 ---------- */
    void*    lastCallInstance;      /* 用于验证 instance 透传 */
    uint32_t writeLen;
    uint32_t writeTimeoutMs;
    uint8_t  writeData[8];          /* 只保存前 8 字节，足够本文件用 */
    uint32_t readLen;
    uint32_t readTimeoutMs;
    uint8_t* readBuffer;
    uint32_t lastBaudRate;

    /* ---------- 预设返回值（由用例摆放）---------- */
    int initReturn;
    int deinitReturn;
    int writeReturn;
    int readReturn;
    int flushReturn;
    int bytesAvailableValue;
    int setBaudRateReturn;

    /* ---------- 生命周期标记：1 存活，0 已销毁 ---------- */
    int alive;
} FakeUart;

static FakeUart gFake;

/* ============================================================================
 * 计数版分配器（用例「分配器配平」使用），内部仍走 malloc / free
 * ========================================================================== */
static int gAllocCount;
static int gFreeCount;

static void* countingMalloc(size_t size)
{
    ++gAllocCount;
    return malloc(size);
}

static void countingFree(void* p)
{
    ++gFreeCount;
    free(p);
}

/* 清零 fake 状态并摆放一份中性默认返回值（不使用 string.h，避免越界 include）*/
static void fakeReset(void)
{
    FakeUart blank = { 0 };

    gFake = blank;

    gFake.initReturn           = 0;
    gFake.deinitReturn         = 0;
    gFake.writeReturn          = 0;
    gFake.readReturn           = 0;
    gFake.flushReturn          = 0;
    gFake.bytesAvailableValue  = 0;
    gFake.setBaudRateReturn    = 0;

    gAllocCount = 0;
    gFreeCount  = 0;
}

/* ============================================================================
 * fake 接口函数（一律 static；首参 void* instance；名字避开 read/write 等）
 * ========================================================================== */
static int fakeInit(void* instance)
{
    FakeUart* self = (FakeUart*)instance;

    self->lastCallInstance = instance;
    ++self->initCount;
    self->alive = 1;

    return self->initReturn;
}

static int fakeDeinit(void* instance)
{
    FakeUart* self = (FakeUart*)instance;

    self->lastCallInstance = instance;
    ++self->deinitCount;

    return self->deinitReturn;
}

static int fakeWrite(void* instance, const uint8_t* data, uint32_t len, uint32_t timeoutMs)
{
    FakeUart* self = (FakeUart*)instance;
    uint32_t  i;

    self->lastCallInstance = instance;
    ++self->writeCount;
    self->writeLen       = len;
    self->writeTimeoutMs = timeoutMs;

    if (NULL != data) {
        for (i = 0; (i < len) && (i < (uint32_t)sizeof(self->writeData)); ++i) {
            self->writeData[i] = data[i];
        }
    }

    return self->writeReturn;
}

static int fakeRead(void* instance, uint8_t* buffer, uint32_t len, uint32_t timeoutMs)
{
    FakeUart* self = (FakeUart*)instance;
    uint32_t  i;

    self->lastCallInstance = instance;
    ++self->readCount;
    self->readLen       = len;
    self->readTimeoutMs = timeoutMs;
    self->readBuffer    = buffer;

    if (NULL != buffer) {
        for (i = 0; i < len; ++i) {
            buffer[i] = 0xAA;
        }
    }

    return self->readReturn;
}

static int fakeFlush(void* instance)
{
    FakeUart* self = (FakeUart*)instance;

    self->lastCallInstance = instance;
    ++self->flushCount;

    return self->flushReturn;
}

static int fakeBytesAvailable(void* instance)
{
    FakeUart* self = (FakeUart*)instance;

    self->lastCallInstance = instance;
    ++self->bytesAvailableCount;

    return self->bytesAvailableValue;
}

static int fakeSetBaudRate(void* instance, uint32_t baudRate)
{
    FakeUart* self = (FakeUart*)instance;

    self->lastCallInstance = instance;
    ++self->setBaudRateCount;
    self->lastBaudRate = baudRate;

    return self->setBaudRateReturn;
}

/* 只登记销毁行为，不释放非堆内存（gFake 为静态对象）*/
static void fakeDeleteInstance(void* instance)
{
    FakeUart* self = (FakeUart*)instance;

    self->lastCallInstance = instance;
    ++self->deleteCount;
    self->alive = 0;
}

/* 聚合初始化返回整张虚表（不使用 _RETURN_UART_INTERFACE_WITH 宏）*/
static UartInterface fakeInterface(void)
{
    UartInterface ifaces = {
        .init           = fakeInit,
        .deinit         = fakeDeinit,
        .write          = fakeWrite,
        .read           = fakeRead,
        .flush          = fakeFlush,
        .bytesAvailable = fakeBytesAvailable,
        .setBaudRate    = fakeSetBaudRate,
        .deleteInstance = fakeDeleteInstance,
    };

    return ifaces;
}

/* 便捷构造：把 gFake 装配成抽象 Uart（内部走 NewUart 抽象构造）*/
static Uart* fakeUartNew(void)
{
    return NewUart(&gFake, fakeInterface());
}

/* ============================================================================
 * 用例 ①：UartDefaultConfig(115200) 默认值
 * ========================================================================== */
static void test_uart_default_config(void)
{
    UartConfig config;

    fakeReset();

    config = UartDefaultConfig(115200);

    TEST_ASSERT_EQUAL_UINT32(115200, config.baudRate);
    TEST_ASSERT_EQUAL_INT(UartDataBits8, config.dataBits);
    TEST_ASSERT_EQUAL_INT(UartParityNone, config.parity);
    TEST_ASSERT_EQUAL_INT(UartStopBits1, config.stopBits);
    TEST_ASSERT_EQUAL_INT(UartFlowNone, config.flowControl);
    /* <=0 表示交给平台默认值，不锁定具体数值 */
    TEST_ASSERT_TRUE(config.rxBufferSize <= 0);
    TEST_ASSERT_TRUE(config.txBufferSize <= 0);

    fakeReset();
}

/* ============================================================================
 * 用例 ②：NewUart 拒绝非法入参 + 正常装配
 * ========================================================================== */
static void test_uart_new_rejects_invalid_instance(void)
{
    UartInterface noDeleter;
    Uart*         uart = NULL;

    fakeReset();

    noDeleter = fakeInterface();
    noDeleter.deleteInstance = NULL;    /* 缺析构项：必须拒绝 */

    /* instance 为 NULL */
    TEST_ASSERT_NULL(NewUart(NULL, fakeInterface()));
    /* deleteInstance 为 NULL */
    TEST_ASSERT_NULL(NewUart(&gFake, noDeleter));
    /* 两者皆 NULL */
    TEST_ASSERT_NULL(NewUart(NULL, noDeleter));
    /* 构造失败不得调用任何具象接口 */
    TEST_ASSERT_EQUAL_INT(0, gFake.deleteCount);
    TEST_ASSERT_EQUAL_INT(0, gFake.initCount);

    /* 正常装配 */
    uart = fakeUartNew();
    TEST_ASSERT_NOT_NULL(uart);
    TEST_ASSERT_EQUAL_PTR(&gFake, uart->instance);
    TEST_ASSERT_TRUE(uart->interfaces.write == fakeWrite);
    TEST_ASSERT_TRUE(uart->interfaces.deleteInstance == fakeDeleteInstance);
    /* 仅装配，不得触发任何调用 */
    TEST_ASSERT_EQUAL_INT(0, gFake.initCount);
    TEST_ASSERT_EQUAL_INT(0, gFake.deleteCount);

    DeleteUart(&uart);
    TEST_ASSERT_NULL(uart);

    fakeReset();
}

/* ============================================================================
 * 用例 ③：DeleteUart 语义（deleteInstance 恰好一次、置空、NULL 安全）
 * ========================================================================== */
static void test_uart_delete_calls_delete_instance_once(void)
{
    Uart* uart = NULL;

    fakeReset();

    uart = fakeUartNew();
    TEST_ASSERT_NOT_NULL(uart);
    TEST_ASSERT_EQUAL_INT(0, gFake.deleteCount);
    gFake.alive = 1;

    DeleteUart(&uart);

    TEST_ASSERT_EQUAL_INT(1, gFake.deleteCount);    /* 恰好一次 */
    TEST_ASSERT_EQUAL_INT(0, gFake.alive);          /* 具象实例被销毁 */
    TEST_ASSERT_NULL(uart);                         /* 外部指针已置空 */

    /* 重复调用（uart 已为 NULL）不得重复销毁 */
    DeleteUart(&uart);
    TEST_ASSERT_EQUAL_INT(1, gFake.deleteCount);
    TEST_ASSERT_NULL(uart);

    /* 句柄指针本身为 NULL 时不得崩溃 */
    DeleteUart(NULL);
    TEST_ASSERT_EQUAL_INT(1, gFake.deleteCount);

    fakeReset();
}

/* ============================================================================
 * 用例 ④：空句柄 / 非法入参安全（全部 -1，且具象调用计数不变）
 * ========================================================================== */
static void test_uart_null_and_illegal_args_are_safe(void)
{
    Uart*   uart = NULL;
    uint8_t buffer[4] = { 0 };
    uint8_t sample[4] = { 0x01, 0x02, 0x03, 0x04 };

    fakeReset();

    uart = fakeUartNew();
    TEST_ASSERT_NOT_NULL(uart);
    fakeReset();    /* 构造过程不计数，这里再清一次，确保计数基线为 0 */

    /* --- 空句柄 --- */
    TEST_ASSERT_EQUAL_INT(-1, UartInit(NULL));
    TEST_ASSERT_EQUAL_INT(-1, UartDeinit(NULL));
    TEST_ASSERT_EQUAL_INT(-1, UartFlush(NULL));
    TEST_ASSERT_EQUAL_INT(-1, UartBytesAvailable(NULL));
    TEST_ASSERT_EQUAL_INT(-1, UartWrite(NULL, sample, 4, 10));
    TEST_ASSERT_EQUAL_INT(-1, UartRead(NULL, buffer, 4, 10));
    TEST_ASSERT_EQUAL_INT(-1, UartSetBaudRate(NULL, 115200));

    /* --- 非法入参（句柄有效）--- */
    TEST_ASSERT_EQUAL_INT(-1, UartWrite(uart, NULL, 4, 10));    /* data 为 NULL */
    TEST_ASSERT_EQUAL_INT(-1, UartWrite(uart, sample, 0, 10));  /* len 为 0 */
    TEST_ASSERT_EQUAL_INT(-1, UartRead(uart, NULL, 4, 0));      /* buffer 为 NULL */
    TEST_ASSERT_EQUAL_INT(-1, UartRead(uart, buffer, 0, 0));    /* len 为 0 */
    TEST_ASSERT_EQUAL_INT(-1, UartSetBaudRate(uart, 0));        /* 波特率为 0 */

    /* 以上均不得触碰具象实现 */
    TEST_ASSERT_EQUAL_INT(0, gFake.initCount);
    TEST_ASSERT_EQUAL_INT(0, gFake.deinitCount);
    TEST_ASSERT_EQUAL_INT(0, gFake.writeCount);
    TEST_ASSERT_EQUAL_INT(0, gFake.readCount);
    TEST_ASSERT_EQUAL_INT(0, gFake.flushCount);
    TEST_ASSERT_EQUAL_INT(0, gFake.bytesAvailableCount);
    TEST_ASSERT_EQUAL_INT(0, gFake.setBaudRateCount);
    TEST_ASSERT_EQUAL_INT(0, gFake.deleteCount);

    DeleteUart(&uart);
    TEST_ASSERT_NULL(uart);

    fakeReset();
}

/* ============================================================================
 * 用例 ⑤：透传正确性 —— write / read / setBaudRate
 * ========================================================================== */
static void test_uart_passthrough_write_read_baud(void)
{
    Uart*   uart = NULL;
    uint8_t data[3] = { 0xA5, 0x5A, 0x3C };
    uint8_t buffer[2] = { 0x00, 0x00 };

    fakeReset();

    gFake.writeReturn       = 3;
    gFake.readReturn        = 2;
    gFake.setBaudRateReturn = 0;

    uart = fakeUartNew();
    TEST_ASSERT_NOT_NULL(uart);

    /* --- write：参数与返回值一致 --- */
    TEST_ASSERT_EQUAL_INT(3, UartWrite(uart, data, 3, 25));
    TEST_ASSERT_EQUAL_INT(1, gFake.writeCount);
    TEST_ASSERT_EQUAL_UINT32(3, gFake.writeLen);
    TEST_ASSERT_EQUAL_UINT32(25, gFake.writeTimeoutMs);
    TEST_ASSERT_EQUAL_UINT8(data[0], gFake.writeData[0]);
    TEST_ASSERT_EQUAL_UINT8(data[1], gFake.writeData[1]);
    TEST_ASSERT_EQUAL_UINT8(data[2], gFake.writeData[2]);
    /* instance 透传正确：具象拿到的就是抽象对象里存的指针 */
    TEST_ASSERT_TRUE(gFake.lastCallInstance == (void*)&gFake);

    /* write 返回负值时原样上抛 */
    gFake.writeReturn = -5;
    TEST_ASSERT_EQUAL_INT(-5, UartWrite(uart, data, 1, 0));
    TEST_ASSERT_EQUAL_INT(2, gFake.writeCount);
    gFake.writeReturn = 3;

    /* --- read：timeoutMs == 0 表示非阻塞，必须原样透传 --- */
    TEST_ASSERT_EQUAL_INT(2, UartRead(uart, buffer, 2, 0));
    TEST_ASSERT_EQUAL_INT(1, gFake.readCount);
    TEST_ASSERT_EQUAL_UINT32(2, gFake.readLen);
    TEST_ASSERT_EQUAL_UINT32(0, gFake.readTimeoutMs);
    TEST_ASSERT_TRUE(gFake.readBuffer == buffer);
    TEST_ASSERT_EQUAL_UINT8(0xAA, buffer[0]);   /* fake 填充，证明写入落到同一缓冲 */
    TEST_ASSERT_EQUAL_UINT8(0xAA, buffer[1]);

    /* --- setBaudRate：新波特率透传 --- */
    TEST_ASSERT_EQUAL_INT(0, UartSetBaudRate(uart, 921600));
    TEST_ASSERT_EQUAL_INT(1, gFake.setBaudRateCount);
    TEST_ASSERT_EQUAL_UINT32(921600, gFake.lastBaudRate);

    DeleteUart(&uart);
    TEST_ASSERT_NULL(uart);

    fakeReset();
}

/* ============================================================================
 * 用例 ⑥：透传正确性 —— init / deinit / flush / bytesAvailable
 * ========================================================================== */
static void test_uart_passthrough_lifecycle(void)
{
    Uart* uart = NULL;

    fakeReset();

    gFake.initReturn          = 0;
    gFake.deinitReturn        = 0;
    gFake.flushReturn         = 0;
    gFake.bytesAvailableValue = 17;

    uart = fakeUartNew();
    TEST_ASSERT_NOT_NULL(uart);

    TEST_ASSERT_EQUAL_INT(0, UartInit(uart));
    TEST_ASSERT_EQUAL_INT(1, gFake.initCount);
    TEST_ASSERT_EQUAL_INT(1, gFake.alive);

    TEST_ASSERT_EQUAL_INT(17, UartBytesAvailable(uart));
    TEST_ASSERT_EQUAL_INT(1, gFake.bytesAvailableCount);

    TEST_ASSERT_EQUAL_INT(0, UartFlush(uart));
    TEST_ASSERT_EQUAL_INT(1, gFake.flushCount);

    TEST_ASSERT_EQUAL_INT(0, UartDeinit(uart));
    TEST_ASSERT_EQUAL_INT(1, gFake.deinitCount);

    /* 具象返回负值时原样上抛 */
    gFake.initReturn = -3;
    TEST_ASSERT_EQUAL_INT(-3, UartInit(uart));
    TEST_ASSERT_EQUAL_INT(2, gFake.initCount);
    gFake.initReturn = 0;

    DeleteUart(&uart);
    TEST_ASSERT_NULL(uart);

    fakeReset();
}

/* ============================================================================
 * 用例 ⑦：虚表缺项返回 -2（不是 -1：入参合法但实现缺失）
 * ========================================================================== */
static void test_uart_missing_vtable_entry_returns_minus_two(void)
{
    UartInterface partial;
    Uart*         uart = NULL;
    uint8_t       buffer[2] = { 0 };
    uint8_t       sample[2] = { 0x11, 0x22 };

    fakeReset();

    partial = fakeInterface();
    partial.write          = NULL;
    partial.read           = NULL;
    partial.flush          = NULL;
    partial.bytesAvailable = NULL;
    partial.setBaudRate    = NULL;
    partial.init           = NULL;
    partial.deinit         = NULL;

    uart = NewUart(&gFake, partial);
    TEST_ASSERT_NOT_NULL(uart);

    TEST_ASSERT_EQUAL_INT(-2, UartInit(uart));
    TEST_ASSERT_EQUAL_INT(-2, UartDeinit(uart));
    TEST_ASSERT_EQUAL_INT(-2, UartWrite(uart, sample, 2, 0));
    TEST_ASSERT_EQUAL_INT(-2, UartRead(uart, buffer, 2, 0));
    TEST_ASSERT_EQUAL_INT(-2, UartFlush(uart));
    TEST_ASSERT_EQUAL_INT(-2, UartBytesAvailable(uart));
    TEST_ASSERT_EQUAL_INT(-2, UartSetBaudRate(uart, 115200));

    /* 缺项时同样不得触碰具象实现 */
    TEST_ASSERT_EQUAL_INT(0, gFake.initCount);
    TEST_ASSERT_EQUAL_INT(0, gFake.writeCount);
    TEST_ASSERT_EQUAL_INT(0, gFake.readCount);

    DeleteUart(&uart);
    TEST_ASSERT_NULL(uart);

    fakeReset();
}

/* ============================================================================
 * 用例 ⑧：分配器配平 —— NewUart + DeleteUart 后分配次数 == 释放次数
 * ========================================================================== */
static void test_uart_allocator_balance(void)
{
    void* (*savedMalloc)(size_t size) = uartMalloc;
    void  (*savedFree)(void* p)       = uartFree;
    Uart*   uart  = NULL;
    Uart*   probe = NULL;

    fakeReset();

    /* 换成计数版钩子（内部仍 malloc/free），选到一个不受干扰的基线 */
    uartMalloc  = countingMalloc;
    uartFree    = countingFree;
    gAllocCount = 0;
    gFreeCount  = 0;

    uart = NewUart(&gFake, fakeInterface());
    DeleteUart(&uart);

    /* 先还原钩子再断言，避免断言失败导致全局钩子未复位 */
    uartMalloc = savedMalloc;
    uartFree   = savedFree;

    TEST_ASSERT_NULL(uart);
    TEST_ASSERT_EQUAL_INT(1, gAllocCount);      /* 抽象对象分配 1 次 */
    TEST_ASSERT_EQUAL_INT(1, gFreeCount);       /* 抽象对象释放 1 次 */
    TEST_ASSERT_EQUAL_INT(gAllocCount, gFreeCount);
    TEST_ASSERT_EQUAL_INT(1, gFake.deleteCount);

    /* 还原有效性验证：再走一轮，计数版钩子不应再被调用 */
    probe = NewUart(&gFake, fakeInterface());
    TEST_ASSERT_NOT_NULL(probe);
    DeleteUart(&probe);
    TEST_ASSERT_NULL(probe);
    TEST_ASSERT_EQUAL_INT(1, gAllocCount);
    TEST_ASSERT_EQUAL_INT(1, gFreeCount);
    TEST_ASSERT_TRUE(uartMalloc == savedMalloc);
    TEST_ASSERT_TRUE(uartFree == savedFree);

    fakeReset();
}

/* ============================================================================
 * 用例入口：test_main.c 调用 RunUartTests()
 * ========================================================================== */
void RunUartTests(void)
{
    RUN_TEST(test_uart_default_config);
    RUN_TEST(test_uart_new_rejects_invalid_instance);
    RUN_TEST(test_uart_delete_calls_delete_instance_once);
    RUN_TEST(test_uart_null_and_illegal_args_are_safe);
    RUN_TEST(test_uart_passthrough_write_read_baud);
    RUN_TEST(test_uart_passthrough_lifecycle);
    RUN_TEST(test_uart_missing_vtable_entry_returns_minus_two);
    RUN_TEST(test_uart_allocator_balance);
}
