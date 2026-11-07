#include "uart.h"
#include <stdlib.h>

// ============================================================================
// hal/uart —— UART 抽象层（父类）实现
//
// 本文件只负责：模块级内存钩子的默认实现、抽象类构造/析构、配置默认值、
// 以及把 UartXxx 包装函数透传给具体实现（interfaces）。
// 严禁在此创建任何具体实例，严禁包含 freertos/*、driver/*、esp_*.h。
// ============================================================================

// ===== 模块级内存管理默认实现（可被外部覆盖，如换成 pvPortMalloc/vPortFree）=====
void* (*uartMalloc)(size_t size) = malloc;
void  (*uartFree)(void* p)       = free;

// ===== 抽象类构造：只做「分配 + 存 instance + 存 interfaces」=====
Uart* NewUart(void* instance, UartInterface interfaces) {
    // 参数非法：具体实例与析构函数必须存在（否则 DeleteUart 会泄漏具体实例）
    if (NULL == instance || NULL == interfaces.deleteInstance) return NULL;

    Uart* self = (Uart*)uartMalloc(sizeof(Uart));
    if (NULL == self) return NULL;

    self->instance   = instance;
    self->interfaces = interfaces;

    return self;
}

// ===== 抽象类析构：先销毁具体实例，再释放抽象对象，最后置空防悬空 =====
void DeleteUart(Uart** pUart) {
    if (NULL == pUart || NULL == *pUart) return;

    Uart* self = *pUart;

    // ① 销毁具体实例
    if (NULL != self->instance && NULL != self->interfaces.deleteInstance) {
        self->interfaces.deleteInstance(self->instance);
    }
    self->instance = NULL;

    // ② 释放抽象对象本身
    uartFree(self);

    // ③ 外部指针置空，防止悬空
    *pUart = NULL;
}

// ===== 配置默认值：8N1、无流控、收发缓冲区使用平台默认 =====
UartConfig UartDefaultConfig(uint32_t baudRate) {
    UartConfig config;
    config.baudRate     = baudRate;
    config.dataBits     = UartDataBits8;
    config.parity       = UartParityNone;
    config.stopBits     = UartStopBits1;
    config.flowControl  = UartFlowNone;
    config.rxBufferSize = 0;    // <=0 表示使用平台默认
    config.txBufferSize = 0;    // <=0 表示使用平台默认
    return config;
}

// ===== 抽象接口包装：先判空/判非法入参，再透传 =====
int UartInit(Uart* uart) {
    if (NULL == uart || NULL == uart->instance) return -1;
    if (NULL == uart->interfaces.init)          return -2;
    return uart->interfaces.init(uart->instance);
}

int UartDeinit(Uart* uart) {
    if (NULL == uart || NULL == uart->instance) return -1;
    if (NULL == uart->interfaces.deinit)        return -2;
    return uart->interfaces.deinit(uart->instance);
}

int UartWrite(Uart* uart, const uint8_t* data, uint32_t len, uint32_t timeoutMs) {
    if (NULL == uart || NULL == uart->instance || NULL == data || 0 == len) return -1;
    if (NULL == uart->interfaces.write)                                     return -2;
    return uart->interfaces.write(uart->instance, data, len, timeoutMs);
}

int UartRead(Uart* uart, uint8_t* buffer, uint32_t len, uint32_t timeoutMs) {
    if (NULL == uart || NULL == uart->instance || NULL == buffer || 0 == len) return -1;
    if (NULL == uart->interfaces.read)                                        return -2;
    return uart->interfaces.read(uart->instance, buffer, len, timeoutMs);
}

int UartFlush(Uart* uart) {
    if (NULL == uart || NULL == uart->instance) return -1;
    if (NULL == uart->interfaces.flush)         return -2;
    return uart->interfaces.flush(uart->instance);
}

int UartBytesAvailable(Uart* uart) {
    if (NULL == uart || NULL == uart->instance) return -1;
    if (NULL == uart->interfaces.bytesAvailable) return -2;
    return uart->interfaces.bytesAvailable(uart->instance);
}

int UartSetBaudRate(Uart* uart, uint32_t baudRate) {
    if (NULL == uart || NULL == uart->instance || 0 == baudRate) return -1;
    if (NULL == uart->interfaces.setBaudRate)                    return -2;
    return uart->interfaces.setBaudRate(uart->instance, baudRate);
}
