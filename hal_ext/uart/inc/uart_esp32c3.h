/**
 * @file uart_esp32c3.h
 * @brief UART 具象派生类（ESP32-C3 / ESP-IDF v5.x）声明。
 *
 * ============================================================
 * OOP in C 关系
 * ============================================================
 * 抽象父类 Uart（uart.h）
 *     { void* instance; UartInterface interfaces; }
 *              │ instance
 *              ▼
 * 具象派生类 UartEsp32c3（本文件）
 *     { UartConfig config; uint32_t port; int32_t txPin/rxPin; uint8_t isInstalled; }
 *
 * 上层（main / service）只依赖 uart.h 的 Uart*，通过 UartWrite / UartRead ...
 * 等包装函数调用；运行时实际执行的是 uart_esp32c3.c 里的静态接口实现。
 *
 * ============================================================
 * 头文件约束（强制）
 * ============================================================
 * 1. 本文件不得包含任何 ESP-IDF / FreeRTOS 头（driver 与 freertos 下的头文件、
 *    esp_*.h），保证使用方不被拖入平台头文件链；
 * 2. 结构体字段只用基本整型（uint32_t / int32_t / uint8_t）与抽象头
 *    中的纯数据类型 UartConfig；
 * 3. uart_port_t、uart_config_t、esp_err_t 等平台类型仅允许出现在
 *    uart_esp32c3.c 内部。
 *
 * ============================================================
 * UART0 提醒
 * ============================================================
 * ESP32-C3 的 UART0 默认被 ESP-IDF 控制台（stdout / 启动日志）占用。
 * 本模块不会改动任何控制台配置；用 port = 0 创建业务串口前请先确认
 * CONFIG_ESP_CONSOLE_UART_NUM，业务串口通常应使用 port = 1。
 */

#ifndef _UART_ESP32C3_H
#define _UART_ESP32C3_H

#include "uart.h"   /* 抽象父类：Uart / UartInterface / UartConfig / UartDefaultConfig */

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * 具象派生类实例结构体（由 uart_esp32c3.c 内部填充与解释）
 * ============================================================ */
typedef struct UartEsp32c3 {
    UartConfig config;      /*!< 当前串口配置（枚举与整型均来自抽象头，无平台类型） */
    uint32_t   port;        /*!< UART 端口号：0 = UART0，1 = UART1（ESP32-C3 仅两路） */
    int32_t    txPin;       /*!< TX 引脚号；-1 表示保持平台默认（内部映射 UART_PIN_NO_CHANGE） */
    int32_t    rxPin;       /*!< RX 引脚号；-1 表示保持平台默认（内部映射 UART_PIN_NO_CHANGE） */
    uint8_t    isInstalled; /*!< 1 = 驱动已安装（uart_driver_install 成功），0 = 未安装 */
} UartEsp32c3;

/* ============================================================
 * 具体构造 / 接口表
 * ============================================================ */

/**
 * @brief 具体构造：创建 ESP32-C3 的 UART 对象（内部完成硬件初始化）。
 *
 * 构造流程：uartMalloc → memset → 保存 port/config/引脚 →
 *           uart_param_config + uart_set_pin + uart_driver_install →
 *           NewUart(ins, UartEsp32c3Interface()) 完成抽象装配。
 *
 * @param port    UART 端口号，仅 0 / 1 合法；>= UART_NUM_MAX 时返回 NULL。
 * @param config  串口配置；传 NULL 时按 115200-8N1、无流控、默认缓冲区处理
 *                （等价于 UartDefaultConfig(115200)）。
 *                rxBufferSize <= 0 → 使用 1024；txBufferSize <= 0 → 使用 0（IDF 允许 0）。
 * @param txPin   TX 引脚号；-1 表示保持平台默认引脚（UART_PIN_NO_CHANGE），< -1 非法。
 * @param rxPin   RX 引脚号；-1 表示保持平台默认引脚（UART_PIN_NO_CHANGE），< -1 非法。
 *
 * @return 成功返回抽象父类指针 Uart*（由 DeleteUart 统一销毁）；
 *         参数非法（port 越界 / 引脚非法 / flowControl 为 UartFlowDtrDsr）、
 *         端口已被其它模块安装过驱动、内存不足或硬件初始化失败时返回 NULL。
 *
 * @note 平台限制：ESP32-C3 的 UART 外设只支持无流控与 RTS/CTS 硬件流控，
 *       不支持 DTR/DSR，因此 config->flowControl == UartFlowDtrDsr 会被直接拒绝
 *       （返回 NULL），而不是静默降级为无流控。
 */
Uart* NewUartEsp32c3(uint32_t port, const UartConfig* config, int32_t txPin, int32_t rxPin);

/**
 * @brief 返回 ESP32-C3 具象实现的接口表（虚函数表）。
 *
 * 字段顺序严格对应 uart.h 的 UartInterface：
 *     init / deinit / write / read / flush / bytesAvailable / setBaudRate / deleteInstance
 *
 * @return 填满全部函数指针的 UartInterface（deleteInstance 保证非 NULL）。
 */
UartInterface UartEsp32c3Interface(void);

#ifdef __cplusplus
}
#endif

#endif /* _UART_ESP32C3_H */
