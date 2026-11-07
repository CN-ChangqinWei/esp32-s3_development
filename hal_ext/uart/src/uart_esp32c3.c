/**
 * @file uart_esp32c3.c
 * @brief UART 具象派生类（ESP32-C3 / ESP-IDF v5.5.x，driver/uart.h）实现。
 *
 * ============================================================
 * 文件结构
 * ============================================================
 *   ① 平台常量与错误码
 *   ② 抽象枚举 → ESP-IDF 枚举映射（纯函数，无副作用）
 *   ③ 硬件安装 / 卸载（uart_param_config + uart_set_pin + uart_driver_install）
 *   ④ 虚表接口实现（全部 static，首参恒为 void* instance，首行取 self）
 *   ⑤ 接口表 UartEsp32c3Interface（_RETURN_UART_INTERFACE_WITH）
 *   ⑥ 具体构造 NewUartEsp32c3（内部调用抽象构造 NewUart 完成装配）
 *
 * ============================================================
 * 【重要提醒】UART0 与 IDF 控制台
 * ============================================================
 * ESP32-C3 的 UART0 默认被 ESP-IDF 控制台占用（CONFIG_ESP_CONSOLE_UART_NUM
 * 默认 0），启动日志与 printf 都走它。本文件刻意不修改任何控制台行为
 * （不改 uart_vfs / console 配置，也不调用 esp_vfs / esp_rom 相关接口）。
 * 使用者若用 NewUartEsp32c3(0, ...) 建业务串口，出现日志丢失或打印错乱时：
 *   ○ 业务串口请改用 UART1（port = 1）；或
 *   ○ 在 menuconfig 中把控制台切到 USB Serial/JTAG，再释放 UART0。
 *
 * ============================================================
 * 内存与依赖约定
 * ============================================================
 *   ○ 对象内存一律走抽象层的 uartMalloc / uartFree（默认 malloc / free，
 *     可被外部替换为 pvPortMalloc / vPortFree）；
 *   ○ 本文件是唯一允许出现 ESP-IDF / FreeRTOS 类型的地方，抽象头保持纯净。
 */

#include "uart_esp32c3.h"

#include <string.h>

#include "driver/uart.h"        /* 仅具象层包含平台驱动头 */
#include "esp_err.h"
#include "freertos/FreeRTOS.h"  /* pdMS_TO_TICKS */

/* ============================================================
 * ① 平台常量与错误码
 * ============================================================ */

/* 构造默认值（config == NULL 时使用） */
#define UART_ESP32C3_DEFAULT_BAUD_RATE      (115200u)
#define UART_ESP32C3_DEFAULT_RX_BUFFER_SIZE (1024)
#define UART_ESP32C3_DEFAULT_TX_BUFFER_SIZE (0)     /* IDF 允许 0：不建 TX 环形缓冲 */

/* 返回码：与抽象层约定一致（0 成功 / -1 参数非法 / 其余负值为平台失败） */
#define UART_ESP32C3_OK          (0)
#define UART_ESP32C3_ERR_PARAM   (-1)   /* 参数非法或对象未就绪（驱动未安装） */
#define UART_ESP32C3_ERR_DRIVER  (-3)   /* ESP-IDF 驱动返回失败 */
#define UART_ESP32C3_ERR_TIMEOUT (-4)   /* 发送在限定时间内未完成（数据已进入 TX 缓冲） */

/* ============================================================
 * ② 抽象枚举 → ESP-IDF 枚举映射
 *    说明：抽象头使用 CamelCase 枚举名，避免与 IDF 的
 *    UART_DATA_8_BITS / UART_PARITY_EVEN 等全大写枚举名冲突。
 * ============================================================ */

/* 数据位：越界值统一按 8 位处理（构造阶段已保证枚举合法） */
static uart_word_length_t uartEsp32c3MapDataBits(UartDataBits dataBits)
{
    switch (dataBits) {
        case UartDataBits5: return UART_DATA_5_BITS;
        case UartDataBits6: return UART_DATA_6_BITS;
        case UartDataBits7: return UART_DATA_7_BITS;
        case UartDataBits8:
        default:            return UART_DATA_8_BITS;
    }
}

/* 校验位：UartParityNone/Even/Odd → UART_PARITY_DISABLE/EVEN/ODD */
static uart_parity_t uartEsp32c3MapParity(UartParity parity)
{
    switch (parity) {
        case UartParityEven: return UART_PARITY_EVEN;
        case UartParityOdd:  return UART_PARITY_ODD;
        case UartParityNone:
        default:             return UART_PARITY_DISABLE;
    }
}

/* 停止位：UartStopBits1/1_5/2 → UART_STOP_BITS_1/1_5/2 */
static uart_stop_bits_t uartEsp32c3MapStopBits(UartStopBits stopBits)
{
    switch (stopBits) {
        case UartStopBits1_5: return UART_STOP_BITS_1_5;
        case UartStopBits2:   return UART_STOP_BITS_2;
        case UartStopBits1:
        default:              return UART_STOP_BITS_1;
    }
}

/* 流控：UartFlowNone/RtsCts → UART_HW_FLOWCTRL_DISABLE/CTS_RTS
 * 注意：ESP-IDF（v5.5.x）提供的枚举只有 DISABLE / RTS / CTS / CTS_RTS，
 *       没有 UART_HW_FLOWCTRL_RTS_CTS 这个拼写，也没有 DSR/DTR 硬件流控；
 *       CTS_RTS 即「RTS + CTS 双向硬件流控」，语义等价于本层的 UartFlowRtsCts。
 *       UartFlowDtrDsr 平台不支持，NewUartEsp32c3 已提前拒绝（返回 NULL），
 *       此处的 DISABLE 仅作为不可达的防御性分支。 */
static uart_hw_flowcontrol_t uartEsp32c3MapFlowControl(UartFlowControl flowControl)
{
    switch (flowControl) {
        case UartFlowRtsCts: return UART_HW_FLOWCTRL_CTS_RTS;
        case UartFlowNone:
        case UartFlowDtrDsr:
        default:             return UART_HW_FLOWCTRL_DISABLE;
    }
}

/* ============================================================
 * ③ 硬件安装 / 卸载
 * ============================================================ */

/**
 * @brief 按 self 中保存的参数完成硬件初始化（安装驱动）。
 *
 * 顺序固定为「占用校验」→ param_config → set_pin → driver_install：
 *   ○ 先确认端口未被其它模块安装过驱动，避免误改他人的端口配置；
 *   ○ param_config 需要驱动处于未安装状态，先配置再安装；
 *   ○ set_pin 传 UART_PIN_NO_CHANGE 表示保持当前分配的引脚；
 *   ○ driver_install 的 rx_buffer_size 必须大于硬件 FIFO 长度（C3 为 128），
 *     tx_buffer_size 允许为 0（不建 TX 环形缓冲，uart_write_bytes 直接写 FIFO）。
 *
 * @param self 具象实例（非 NULL，由调用方保证）。
 * @return ESP_OK 成功；其余为驱动错误码。
 */
static esp_err_t uartEsp32c3HardwareInstall(UartEsp32c3* self)
{
    uart_port_t   port         = (uart_port_t)self->port;
    uart_config_t uartConfig;
    int           rxBufferSize = self->config.rxBufferSize;
    int           txBufferSize = self->config.txBufferSize;
    esp_err_t     err          = ESP_FAIL;

    /* 端口占用校验：该端口若已被其它模块（例如 user/tools/serial 的 Serial）
     * 安装过驱动，则绝不能继续 uart_param_config——那会把别人的配置改掉，
     * 随后 driver_install 也必然失败。此处直接拒绝，由调用方转成 NULL / -3。
     * 注：本模块自己 deinit 之后的重新 install 不会被误判（驱动已被删除）。 */
    if (uart_is_driver_installed(port)) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 缓冲区大小：<=0 时取默认值（RX 走 1024，TX 走 0） */
    if (rxBufferSize <= 0) {
        rxBufferSize = UART_ESP32C3_DEFAULT_RX_BUFFER_SIZE;
    }
    if (txBufferSize <= 0) {
        txBufferSize = UART_ESP32C3_DEFAULT_TX_BUFFER_SIZE;
    }

    /* 平台配置结构体：先整体清零，再逐字段填充（避免残留字段影响驱动校验） */
    memset(&uartConfig, 0, sizeof(uartConfig));
    uartConfig.baud_rate = (int)self->config.baudRate;
    uartConfig.data_bits = uartEsp32c3MapDataBits(self->config.dataBits);
    uartConfig.parity    = uartEsp32c3MapParity(self->config.parity);
    uartConfig.stop_bits = uartEsp32c3MapStopBits(self->config.stopBits);
    uartConfig.flow_ctrl = uartEsp32c3MapFlowControl(self->config.flowControl);
    /* RTS 触发电平：仅流控开启时需要，取硬件 FIFO 的一半作为折中阈值
     * （IDF 要求该值必须小于 UART_HW_FIFO_LEN，C3 为 128） */
    uartConfig.rx_flow_ctrl_thresh = (UART_HW_FLOWCTRL_DISABLE != uartConfig.flow_ctrl)
                                   ? (uint8_t)(UART_HW_FIFO_LEN(port) / 2)
                                   : 0;
    uartConfig.source_clk          = UART_SCLK_DEFAULT;
    uartConfig.flags.allow_pd      = 0;

    err = uart_param_config(port, &uartConfig);
    if (ESP_OK != err) {
        return err;
    }

    /* 引脚：< 0（-1）即 UART_PIN_NO_CHANGE，保持平台已分配的默认引脚；
     * RTS / CTS 未在本模块暴露，一律保持不变 */
    err = uart_set_pin(port,
                       (int)self->txPin,
                       (int)self->rxPin,
                       UART_PIN_NO_CHANGE,
                       UART_PIN_NO_CHANGE);
    if (ESP_OK != err) {
        return err;
    }

    /* 事件队列不使用：queue_size = 0、uart_queue = NULL、intr_alloc_flags = 0 */
    err = uart_driver_install(port, rxBufferSize, txBufferSize, 0, NULL, 0);
    if (ESP_OK != err) {
        return err;
    }

    self->isInstalled = 1;
    return ESP_OK;
}

/**
 * @brief 卸载驱动（uart_driver_delete），并清掉已安装标记。
 *
 * 只在 isInstalled == 1 时才真正删除，避免误删别人安装的端口
 * （例如未被本模块安装的端口 / 控制台端口）。
 */
static int uartEsp32c3HardwareUninstall(UartEsp32c3* self)
{
    esp_err_t err = ESP_OK;

    if (NULL == self) {
        return UART_ESP32C3_ERR_PARAM;
    }
    if (0 == self->isInstalled) {
        return UART_ESP32C3_OK;     /* 从未安装：幂等成功，绝不误删 */
    }

    err = uart_driver_delete((uart_port_t)self->port);

    /* 无论返回值如何都标记为未安装，防止重复删除同一端口 */
    self->isInstalled = 0;

    return (ESP_OK == err) ? UART_ESP32C3_OK : UART_ESP32C3_ERR_DRIVER;
}

/* 对象就绪校验：实例存在且驱动已安装（deinit 之后所有 I/O 一律返回 -1） */
static int uartEsp32c3CheckReady(const UartEsp32c3* self)
{
    if (NULL == self || 0 == self->isInstalled) {
        return UART_ESP32C3_ERR_PARAM;
    }
    return UART_ESP32C3_OK;
}

/* ============================================================
 * ④ 虚表接口实现（static，首参恒为 void* instance）
 * ============================================================ */

/**
 * @brief 初始化：只做状态标记 / 重装校验，真正的硬件初始化在具体构造里完成。
 *
 *   ○ 已安装 → 直接成功（绝不重复 uart_driver_install，否则会因二次分配
 *     环形缓冲 / 中断而失败）；
 *   ○ 曾 UartDeinit 过 → 驱动已被删除，此处可按保存的参数安全重装。
 */
static int uartEsp32c3Init(void* instance)
{
    UartEsp32c3* self = (UartEsp32c3*)instance;

    if (NULL == self) {
        return UART_ESP32C3_ERR_PARAM;
    }
    if (1 == self->isInstalled) {
        return UART_ESP32C3_OK;
    }

    return (ESP_OK == uartEsp32c3HardwareInstall(self)) ? UART_ESP32C3_OK
                                                        : UART_ESP32C3_ERR_DRIVER;
}

/* 反初始化：删除驱动，幂等 */
static int uartEsp32c3Deinit(void* instance)
{
    UartEsp32c3* self = (UartEsp32c3*)instance;

    if (NULL == self) {
        return UART_ESP32C3_ERR_PARAM;
    }
    return uartEsp32c3HardwareUninstall(self);
}

/**
 * @brief 发送数据：写入驱动发送缓冲，必要时等待发送完成。
 *
 * @return 实际写入字节数（>= 0）；timeoutMs == 0 时不等待（非阻塞）；
 *         -4 表示数据已进入 TX 缓冲但未在 timeoutMs 内发送完毕。
 */
static int uartEsp32c3Write(void* instance, const uint8_t* data, uint32_t len, uint32_t timeoutMs)
{
    UartEsp32c3* self    = (UartEsp32c3*)instance;
    int          written = 0;

    if (NULL == data || 0 == len) {
        return UART_ESP32C3_ERR_PARAM;
    }
    if (UART_ESP32C3_OK != uartEsp32c3CheckReady(self)) {
        return UART_ESP32C3_ERR_PARAM;
    }

    written = uart_write_bytes((uart_port_t)self->port, (const void*)data, (size_t)len);
    if (written < 0) {
        return UART_ESP32C3_ERR_DRIVER;
    }

    if (timeoutMs > 0) {
        if (ESP_OK != uart_wait_tx_done((uart_port_t)self->port, pdMS_TO_TICKS(timeoutMs))) {
            return UART_ESP32C3_ERR_TIMEOUT;
        }
    }

    return written;
}

/**
 * @brief 接收数据：timeoutMs == 0 时立即返回（可能为 0 字节），> 0 时最多阻塞该毫秒数。
 *
 * @return 实际读取字节数（>= 0）；负值为错误。
 */
static int uartEsp32c3Read(void* instance, uint8_t* buffer, uint32_t len, uint32_t timeoutMs)
{
    UartEsp32c3* self    = (UartEsp32c3*)instance;
    int          readLen = 0;

    if (NULL == buffer || 0 == len) {
        return UART_ESP32C3_ERR_PARAM;
    }
    if (UART_ESP32C3_OK != uartEsp32c3CheckReady(self)) {
        return UART_ESP32C3_ERR_PARAM;
    }

    readLen = uart_read_bytes((uart_port_t)self->port,
                              (void*)buffer,
                              len,
                              pdMS_TO_TICKS(timeoutMs));

    return (readLen < 0) ? UART_ESP32C3_ERR_DRIVER : readLen;
}

/* 清空发送 / 接收缓冲 */
static int uartEsp32c3Flush(void* instance)
{
    UartEsp32c3* self = (UartEsp32c3*)instance;

    if (UART_ESP32C3_OK != uartEsp32c3CheckReady(self)) {
        return UART_ESP32C3_ERR_PARAM;
    }

    return (ESP_OK == uart_flush((uart_port_t)self->port)) ? UART_ESP32C3_OK
                                                           : UART_ESP32C3_ERR_DRIVER;
}

/* 可读字节数（驱动 RX 环形缓冲中的字节数） */
static int uartEsp32c3BytesAvailable(void* instance)
{
    UartEsp32c3* self     = (UartEsp32c3*)instance;
    size_t       buffered = 0;

    if (UART_ESP32C3_OK != uartEsp32c3CheckReady(self)) {
        return UART_ESP32C3_ERR_PARAM;
    }
    if (ESP_OK != uart_get_buffered_data_len((uart_port_t)self->port, &buffered)) {
        return UART_ESP32C3_ERR_DRIVER;
    }

    return (int)buffered;
}

/* 运行时改波特率：成功时同步更新 self->config.baudRate */
static int uartEsp32c3SetBaudRate(void* instance, uint32_t baudRate)
{
    UartEsp32c3* self = (UartEsp32c3*)instance;

    if (0 == baudRate) {
        return UART_ESP32C3_ERR_PARAM;
    }
    if (UART_ESP32C3_OK != uartEsp32c3CheckReady(self)) {
        return UART_ESP32C3_ERR_PARAM;
    }
    if (ESP_OK != uart_set_baudrate((uart_port_t)self->port, baudRate)) {
        return UART_ESP32C3_ERR_DRIVER;
    }

    self->config.baudRate = baudRate;
    return UART_ESP32C3_OK;
}

/* 销毁具体实例：先删驱动（仅在确实安装过时才删），再释放具象对象 */
static void uartEsp32c3DeleteInstance(void* instance)
{
    UartEsp32c3* self = (UartEsp32c3*)instance;

    if (NULL == self) {
        return;
    }

    (void)uartEsp32c3HardwareUninstall(self);

    uartFree(self);
    self = NULL;    /* 局部指针置空，防止释放后误用 */
}

/* ============================================================
 * ⑤ 接口表（字段顺序严格对应 uart.h 的 UartInterface）
 * ============================================================ */
UartInterface UartEsp32c3Interface(void)
{
    _RETURN_UART_INTERFACE_WITH(uartEsp32c3Init,
                                uartEsp32c3Deinit,
                                uartEsp32c3Write,
                                uartEsp32c3Read,
                                uartEsp32c3Flush,
                                uartEsp32c3BytesAvailable,
                                uartEsp32c3SetBaudRate,
                                uartEsp32c3DeleteInstance);
}

/* ============================================================
 * ⑥ 具体构造（内部必须调用抽象构造 NewUart 完成装配）
 * ============================================================ */
Uart* NewUartEsp32c3(uint32_t port, const UartConfig* config, int32_t txPin, int32_t rxPin)
{
    UartEsp32c3* self = NULL;
    Uart*        uart = NULL;

    /* ① 端口合法性：ESP32-C3 仅 UART0 / UART1（上限由平台的 UART_NUM_MAX 决定） */
    if (port >= (uint32_t)UART_NUM_MAX) {
        return NULL;
    }

    /* ② 引脚语义：-1 = 保持平台默认（UART_PIN_NO_CHANGE），更小的值非法 */
    if (txPin < -1 || rxPin < -1) {
        return NULL;
    }

    /* ③ 平台不支持 DTR/DSR 硬件流控：静默降级会误导上层，构造阶段直接拒绝 */
    if (NULL != config && UartFlowDtrDsr == config->flowControl) {
        return NULL;
    }

    /* ④ 内存钩子必须有效（默认 malloc / free，可能被外部替换过） */
    if (NULL == uartMalloc || NULL == uartFree) {
        return NULL;
    }

    /* ⑤ 分配并清零具象实例 */
    self = (UartEsp32c3*)uartMalloc(sizeof(UartEsp32c3));
    if (NULL == self) {
        return NULL;
    }
    memset(self, 0, sizeof(UartEsp32c3));

    /* ⑥ 保存构造参数：config 为 NULL → 115200-8N1、无流控、平台默认缓冲区 */
    self->config = (NULL != config) ? (*config)
                                    : UartDefaultConfig(UART_ESP32C3_DEFAULT_BAUD_RATE);
    if (0 == self->config.baudRate) {
        self->config.baudRate = UART_ESP32C3_DEFAULT_BAUD_RATE;
    }
    self->port        = port;
    self->txPin       = txPin;
    self->rxPin       = rxPin;
    self->isInstalled = 0;

    /* ⑦ 硬件初始化（param_config + set_pin + driver_install），失败则释放并返回 NULL */
    if (ESP_OK != uartEsp32c3HardwareInstall(self)) {
        uartFree(self);
        self = NULL;
        return NULL;
    }

    /* ⑧ 抽象构造装配：instance / interfaces 只能由 NewUart 赋值 */
    uart = NewUart((void*)self, UartEsp32c3Interface());
    if (NULL == uart) {
        /* 装配失败：回滚硬件安装并释放具象实例，避免端口被占死 */
        (void)uartEsp32c3HardwareUninstall(self);
        uartFree(self);
        self = NULL;
        return NULL;
    }

    return uart;
}
