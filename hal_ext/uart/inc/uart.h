#ifndef _UART_H
#define _UART_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// hal/uart —— UART 抽象层（父类，纯 C，零平台依赖）
//
// OOP in C 结构：
//   ① UartInterface = 虚函数表（V-Table），每个接口首参恒为 void* instance；
//   ② Uart          = 抽象类结构体，只放 void* instance + UartInterface interfaces；
//   ③ NewUart       = 抽象类构造，只做「分配 + 存 instance + 存 interfaces」，
//                     禁止在内部创建任何具体实例；
//   ④ DeleteUart    = 抽象类析构，先调 deleteInstance 销毁具体实例，
//                     再 uartFree 抽象对象，最后把外部指针置 NULL；
//   ⑤ 具象实现（如 esp32c3）在自己的 .c 中实现接口函数，并提供
//     具体构造 New<Xxx>(...)，其内部必须调用 NewUart 完成装配；
//      接口表用本头文件的 _RETURN_UART_INTERFACE 宏统一返回。
//
// 返回值约定：
//   write / read : 返回实际字节数（>= 0），负值表示错误；
//   其余接口      : 0 成功，-1 参数非法，-2 未实现（函数指针为 NULL）；
//   timeoutMs = 0 表示非阻塞。
//
// 约束：本头文件不得包含 freertos/*、driver/*、esp_*.h（只用 C 标准头），
//       以保证使用方（main 等）只依赖抽象头，不被拖入平台头文件链。
// ============================================================================

// ========== ③ 模块级内存管理接口（默认 malloc / free，可被外部覆盖）==========
extern void* (*uartMalloc)(size_t size);
extern void  (*uartFree)(void* p);

// ========== UART 配置值类型（纯数据，不含任何平台类型）==========
// 注意：枚举常量统一使用 CamelCase，避免与 ESP-IDF driver/uart.h 中的
//       UART_PARITY_EVEN / UART_DATA_8_BITS 等全大写枚举名冲突
//       （具象 .c 会同时包含 driver/uart.h）。
typedef enum UartDataBits {
    UartDataBits5 = 5,
    UartDataBits6,
    UartDataBits7,
    UartDataBits8
} UartDataBits;

typedef enum UartParity {
    UartParityNone = 0,
    UartParityEven,
    UartParityOdd
} UartParity;

typedef enum UartStopBits {
    UartStopBits1 = 0,
    UartStopBits1_5,
    UartStopBits2
} UartStopBits;

typedef enum UartFlowControl {
    UartFlowNone = 0,
    UartFlowRtsCts,
    UartFlowDtrDsr
} UartFlowControl;

typedef struct UartConfig {
    uint32_t        baudRate;       // 波特率
    UartDataBits    dataBits;       // 数据位
    UartParity      parity;         // 校验位
    UartStopBits    stopBits;       // 停止位
    UartFlowControl flowControl;    // 流控
    int             rxBufferSize;   // 接收缓冲区大小，<=0 表示使用平台默认
    int             txBufferSize;   // 发送缓冲区大小，<=0 表示使用平台默认
} UartConfig;

// ========== ① 接口表（虚函数表 V-Table）==========
typedef struct UartInterface {
    int (*init)(void* instance);                                                    // 初始化（0 成功，负值失败）
    int (*deinit)(void* instance);                                                  // 反初始化
    int (*write)(void* instance, const uint8_t* data, uint32_t len, uint32_t timeoutMs); // 返回实际写入字节数或负值
    int (*read)(void* instance, uint8_t* buffer, uint32_t len, uint32_t timeoutMs);      // 返回实际读取字节数或负值
    int (*flush)(void* instance);                                                   // 清空发送/接收缓冲
    int (*bytesAvailable)(void* instance);                                          // 可读字节数（>=0）或负值
    int (*setBaudRate)(void* instance, uint32_t baudRate);                           // 运行时改波特率
    void (*deleteInstance)(void* instance);                                          // 【必需】销毁具体实例
} UartInterface;

// ========== ② 抽象类结构体 ==========
typedef struct Uart {
    void*          instance;    // 指向具体实现的实例（多态核心）
    UartInterface  interfaces;  // 接口函数表（字段名恒为 interfaces）
} Uart;

// ========== 接口表返回宏（供具象 .c 直接使用）==========
// 用法（具象 .c 内，函数体内）：
//     static int init(void* instance) {...}
//     ...
//     UartInterface UartEsp32c3Interface(void) {
//         _RETURN_UART_INTERFACE;
//     }
// 默认要求具象 .c 中的静态函数名恰为：init / deinit / write / read / flush /
// bytesAvailable / setBaudRate / deleteInstance。
// 若与 <unistd.h> 的 read/write 等已有声明冲突，改用带参版本
// _RETURN_UART_INTERFACE_WITH(...) 传入自己的函数名即可。
#define _RETURN_UART_INTERFACE_WITH(initFunc, deinitFunc, writeFunc, readFunc, flushFunc, bytesAvailableFunc, setBaudRateFunc, deleteFunc) \
    UartInterface interfaces = {                                  \
        .init           = (initFunc),                              \
        .deinit         = (deinitFunc),                            \
        .write          = (writeFunc),                             \
        .read           = (readFunc),                              \
        .flush          = (flushFunc),                             \
        .bytesAvailable = (bytesAvailableFunc),                    \
        .setBaudRate    = (setBaudRateFunc),                       \
        .deleteInstance = (deleteFunc),                            \
    };                                                             \
    return interfaces

#define _RETURN_UART_INTERFACE \
    _RETURN_UART_INTERFACE_WITH(init, deinit, write, read, flush, bytesAvailable, setBaudRate, deleteInstance)

// ========== ④ 抽象类构造 / 析构 ==========
// 抽象构造：只做「分配 + 存 instance + 存 interfaces」，不创建具体实例。
// instance / interfaces.deleteInstance 为 NULL 时返回 NULL。
Uart* NewUart(void* instance, UartInterface interfaces);
// 抽象析构：deleteInstance 销毁具体实例 -> uartFree 抽象对象 -> 置 NULL。
void  DeleteUart(Uart** pUart);

// ========== ⑤ 配置默认值 ==========
// 8N1、无流控、收发缓冲区使用平台默认（<=0）。
UartConfig UartDefaultConfig(uint32_t baudRate);

// ========== ⑥ 抽象接口包装（先判空/判非法入参，再透传给具象实现）==========
int UartInit(Uart* uart);
int UartDeinit(Uart* uart);
int UartWrite(Uart* uart, const uint8_t* data, uint32_t len, uint32_t timeoutMs);
int UartRead(Uart* uart, uint8_t* buffer, uint32_t len, uint32_t timeoutMs);
int UartFlush(Uart* uart);
int UartBytesAvailable(Uart* uart);
int UartSetBaudRate(Uart* uart, uint32_t baudRate);

#ifdef __cplusplus
}
#endif

#endif /* _UART_H */
