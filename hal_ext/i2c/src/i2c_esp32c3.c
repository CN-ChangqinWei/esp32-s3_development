/**
 * @file i2c_esp32c3.c
 * @brief I2C 具象派生类实现：ESP32-C3（ESP-IDF v5.5.x，legacy 主机 API）。
 *
 * 平台映射说明（抽象虚表 -> ESP-IDF legacy API）：
 *   init      : i2c_param_config + i2c_driver_install(I2C_MODE_MASTER, 0, 0, 0)
 *   deinit    : i2c_driver_delete
 *   write     : start -> write_byte(addr<<1|WRITE) -> write -> stop -> cmd_begin
 *   read      : start -> write_byte(addr<<1|READ)  -> read(LAST_NACK) -> stop -> cmd_begin
 *   writeRead : start -> 写段 -> start(重复起始) -> write_byte(<<1|READ) -> 读段 -> stop
 *   probe     : start -> write_byte(addr<<1|WRITE) -> stop -> cmd_begin（0 在线 / -3 无应答）
 *   setClock  : 重新下发 i2c_param_config 的 master.clk_speed（legacy 头无 i2c_set_clock）
 *   reset     : i2c_reset_tx_fifo + i2c_reset_rx_fifo
 *   deleteInstance : i2c_driver_delete（仅当已 install）-> i2cFree
 *
 * 说明：抽象接口「每次调用带 deviceAddress」与 legacy API 天然匹配；
 *       新版 driver/i2c_master.h 需按设备地址缓存句柄，故此处不采用。
 *       legacy API 在 IDF v5.5 仍可用，允许出现 deprecation 警告。
 */

#include "i2c_esp32c3.h"

#include <stdbool.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "driver/i2c.h"

/* 个别 IDF 版本把端口上限放在 soc_caps.h 的 SOC_I2C_NUM，此处做兜底定义 */
#if !defined(I2C_NUM_MAX)
#include "soc/soc_caps.h"
#define I2C_NUM_MAX  SOC_I2C_NUM
#endif

/* legacy I2C API 在新版本中被标记为 deprecated，但本项目有意使用它；
 * 屏蔽该告警，避免 -Werror 构建失败。 */
#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

/* --------------------------------------------------------------------------
 * 返回值常量（与 i2c.h 的对外约定保持一致）
 * -------------------------------------------------------------------------- */
#define I2C_ESP32C3_OK              (0)
#define I2C_ESP32C3_ERR_PARAM       (-1)
#define I2C_ESP32C3_ERR_NACK        (-3)

/* probe 无 timeout 形参，使用固定短超时 */
#define I2C_ESP32C3_PROBE_TIMEOUT_MS (50U)

/* 7 位设备地址上限 */
#define I2C_ESP32C3_ADDR_MAX        (0x7FU)

/* ==========================================================================
 * 内部工具（全部 static，不向上暴露）
 * ========================================================================== */

/* 端口合法性：ESP32-C3 上 I2C_NUM_MAX 为 1，即只允许 port == 0 */
static int i2cEsp32c3CheckPort(uint32_t port)
{
    return (port < (uint32_t)I2C_NUM_MAX) ? I2C_ESP32C3_OK : I2C_ESP32C3_ERR_PARAM;
}

/* 7 位设备地址合法性 */
static int i2cEsp32c3CheckAddress(uint16_t deviceAddress)
{
    return (deviceAddress <= (uint16_t)I2C_ESP32C3_ADDR_MAX) ? I2C_ESP32C3_OK
                                                             : I2C_ESP32C3_ERR_PARAM;
}

/* 毫秒 -> Tick；非 0 超时至少等待 1 个 tick，避免 0 == 无限等待 */
static TickType_t i2cEsp32c3Ticks(uint32_t timeoutMs)
{
    TickType_t ticks = 0;

    if (0U == timeoutMs) {
        return 0;
    }

    ticks = pdMS_TO_TICKS(timeoutMs);
    if (0U == ticks) {
        ticks = 1;
    }

    return ticks;
}

/* ESP-IDF 错误码 -> 本模块返回值：成功 0，其余（NACK / FAIL / TIMEOUT）统一 -3 */
static int i2cEsp32c3MapError(esp_err_t err)
{
    return (ESP_OK == err) ? I2C_ESP32C3_OK : I2C_ESP32C3_ERR_NACK;
}

/* 下发总线参数（引脚 + 时钟），驱动 install 前后均可调用 */
static int i2cEsp32c3ApplyConfig(I2cEsp32c3* self, uint32_t clockHz)
{
    i2c_config_t conf;
    esp_err_t    err;

    if (NULL == self) {
        return I2C_ESP32C3_ERR_PARAM;
    }
    if (0U == clockHz) {
        return I2C_ESP32C3_ERR_PARAM;
    }
    if (I2C_ESP32C3_OK != i2cEsp32c3CheckPort(self->port)) {
        return I2C_ESP32C3_ERR_PARAM;
    }

    memset(&conf, 0, sizeof(conf));
    conf.mode             = I2C_MODE_MASTER;
    conf.sda_io_num       = (int)self->sdaPin;
    conf.scl_io_num       = (int)self->sclPin;
    conf.sda_pullup_en    = true;   /* 内部弱上拉，外部 4.7k 强上拉并存无害 */
    conf.scl_pullup_en    = true;
    conf.master.clk_speed = clockHz;

    err = i2c_param_config((i2c_port_t)self->port, &conf);

    return (ESP_OK == err) ? I2C_ESP32C3_OK : I2C_ESP32C3_ERR_PARAM;
}

/* 安装 legacy 主机驱动：先下发参数，再 install，成功后置 installed 标志 */
static int i2cEsp32c3Install(I2cEsp32c3* self)
{
    esp_err_t err;

    if (NULL == self) {
        return I2C_ESP32C3_ERR_PARAM;
    }
    if (I2C_ESP32C3_OK != i2cEsp32c3ApplyConfig(self, self->clockHz)) {
        return I2C_ESP32C3_ERR_PARAM;
    }

    err = i2c_driver_install((i2c_port_t)self->port, I2C_MODE_MASTER, 0, 0, 0);
    if (ESP_OK != err) {
        return I2C_ESP32C3_ERR_PARAM;
    }

    self->installed = 1;

    return I2C_ESP32C3_OK;
}

/* ==========================================================================
 * 虚表实现：首参恒为 void* instance，首行取回具象实例
 * ========================================================================== */

static int i2cEsp32c3Init(void* instance)
{
    I2cEsp32c3* self = (I2cEsp32c3*)instance;

    if (NULL == self) {
        return I2C_ESP32C3_ERR_PARAM;
    }
    if (I2C_ESP32C3_OK != i2cEsp32c3CheckPort(self->port)) {
        return I2C_ESP32C3_ERR_PARAM;
    }

    /* 幂等：已安装则仅重新下发时钟/引脚参数 */
    if (0U != self->installed) {
        return i2cEsp32c3ApplyConfig(self, self->clockHz);
    }

    return i2cEsp32c3Install(self);
}

static int i2cEsp32c3Deinit(void* instance)
{
    I2cEsp32c3* self = (I2cEsp32c3*)instance;
    esp_err_t    err;

    if (NULL == self) {
        return I2C_ESP32C3_ERR_PARAM;
    }
    if (I2C_ESP32C3_OK != i2cEsp32c3CheckPort(self->port)) {
        return I2C_ESP32C3_ERR_PARAM;
    }
    if (0U == self->installed) {
        return I2C_ESP32C3_OK; /* 未安装视为已卸载（幂等） */
    }

    err = i2c_driver_delete((i2c_port_t)self->port);
    if (ESP_OK != err) {
        return I2C_ESP32C3_ERR_PARAM;
    }

    self->installed = 0;

    return I2C_ESP32C3_OK;
}

static int i2cEsp32c3Write(void* instance, uint16_t deviceAddress,
                           const uint8_t* data, uint32_t len, uint32_t timeoutMs)
{
    I2cEsp32c3*      self = (I2cEsp32c3*)instance;
    i2c_cmd_handle_t cmd  = NULL;
    esp_err_t        err  = ESP_OK;

    if (NULL == self) {
        return I2C_ESP32C3_ERR_PARAM;
    }
    if ((NULL == data) || (0U == len)) {
        return I2C_ESP32C3_ERR_PARAM;
    }
    if (I2C_ESP32C3_OK != i2cEsp32c3CheckPort(self->port)) {
        return I2C_ESP32C3_ERR_PARAM;
    }
    if (I2C_ESP32C3_OK != i2cEsp32c3CheckAddress(deviceAddress)) {
        return I2C_ESP32C3_ERR_PARAM;
    }
    if (0U == self->installed) {
        return I2C_ESP32C3_ERR_PARAM;
    }

    cmd = i2c_cmd_link_create();
    if (NULL == cmd) {
        return I2C_ESP32C3_ERR_PARAM;
    }

    (void)i2c_master_start(cmd);
    (void)i2c_master_write_byte(cmd,
                                (uint8_t)((deviceAddress << 1) | I2C_MASTER_WRITE), true);
    (void)i2c_master_write(cmd, data, (size_t)len, true);
    (void)i2c_master_stop(cmd);

    err = i2c_master_cmd_begin((i2c_port_t)self->port, cmd,
                               i2cEsp32c3Ticks(timeoutMs));
    i2c_cmd_link_delete(cmd);

    return i2cEsp32c3MapError(err);
}

static int i2cEsp32c3Read(void* instance, uint16_t deviceAddress,
                          uint8_t* buffer, uint32_t len, uint32_t timeoutMs)
{
    I2cEsp32c3*      self = (I2cEsp32c3*)instance;
    i2c_cmd_handle_t cmd  = NULL;
    esp_err_t        err  = ESP_OK;

    if (NULL == self) {
        return I2C_ESP32C3_ERR_PARAM;
    }
    if ((NULL == buffer) || (0U == len)) {
        return I2C_ESP32C3_ERR_PARAM;
    }
    if (I2C_ESP32C3_OK != i2cEsp32c3CheckPort(self->port)) {
        return I2C_ESP32C3_ERR_PARAM;
    }
    if (I2C_ESP32C3_OK != i2cEsp32c3CheckAddress(deviceAddress)) {
        return I2C_ESP32C3_ERR_PARAM;
    }
    if (0U == self->installed) {
        return I2C_ESP32C3_ERR_PARAM;
    }

    cmd = i2c_cmd_link_create();
    if (NULL == cmd) {
        return I2C_ESP32C3_ERR_PARAM;
    }

    (void)i2c_master_start(cmd);
    (void)i2c_master_write_byte(cmd,
                                (uint8_t)((deviceAddress << 1) | I2C_MASTER_READ), true);
    (void)i2c_master_read(cmd, buffer, (size_t)len, I2C_MASTER_LAST_NACK);
    (void)i2c_master_stop(cmd);

    err = i2c_master_cmd_begin((i2c_port_t)self->port, cmd,
                               i2cEsp32c3Ticks(timeoutMs));
    i2c_cmd_link_delete(cmd);

    return i2cEsp32c3MapError(err);
}

static int i2cEsp32c3WriteRead(void* instance, uint16_t deviceAddress,
                               const uint8_t* writeData, uint32_t writeLen,
                               uint8_t* readBuffer, uint32_t readLen,
                               uint32_t timeoutMs)
{
    I2cEsp32c3*      self = (I2cEsp32c3*)instance;
    i2c_cmd_handle_t cmd  = NULL;
    esp_err_t        err  = ESP_OK;

    if (NULL == self) {
        return I2C_ESP32C3_ERR_PARAM;
    }
    /* 两侧至少一侧有效；有效侧指针必须非空 */
    if ((0U == writeLen) && (0U == readLen)) {
        return I2C_ESP32C3_ERR_PARAM;
    }
    if ((writeLen > 0U) && (NULL == writeData)) {
        return I2C_ESP32C3_ERR_PARAM;
    }
    if ((readLen > 0U) && (NULL == readBuffer)) {
        return I2C_ESP32C3_ERR_PARAM;
    }
    if (I2C_ESP32C3_OK != i2cEsp32c3CheckPort(self->port)) {
        return I2C_ESP32C3_ERR_PARAM;
    }
    if (I2C_ESP32C3_OK != i2cEsp32c3CheckAddress(deviceAddress)) {
        return I2C_ESP32C3_ERR_PARAM;
    }
    if (0U == self->installed) {
        return I2C_ESP32C3_ERR_PARAM;
    }

    cmd = i2c_cmd_link_create();
    if (NULL == cmd) {
        return I2C_ESP32C3_ERR_PARAM;
    }

    (void)i2c_master_start(cmd);
    (void)i2c_master_write_byte(cmd,
                                (uint8_t)((deviceAddress << 1) | I2C_MASTER_WRITE), true);
    if (writeLen > 0U) {
        (void)i2c_master_write(cmd, writeData, (size_t)writeLen, true);
    }
    if (readLen > 0U) {
        /* 重复起始（restart）后再读，符合绝大多数寄存器式器件的时序 */
        (void)i2c_master_start(cmd);
        (void)i2c_master_write_byte(cmd,
                                    (uint8_t)((deviceAddress << 1) | I2C_MASTER_READ), true);
        (void)i2c_master_read(cmd, readBuffer, (size_t)readLen, I2C_MASTER_LAST_NACK);
    }
    (void)i2c_master_stop(cmd);

    err = i2c_master_cmd_begin((i2c_port_t)self->port, cmd,
                               i2cEsp32c3Ticks(timeoutMs));
    i2c_cmd_link_delete(cmd);

    return i2cEsp32c3MapError(err);
}

/* 设备在线探测：0 在线，-3 无应答 */
static int i2cEsp32c3Probe(void* instance, uint16_t deviceAddress)
{
    I2cEsp32c3*      self = (I2cEsp32c3*)instance;
    i2c_cmd_handle_t cmd  = NULL;
    esp_err_t        err  = ESP_OK;

    if (NULL == self) {
        return I2C_ESP32C3_ERR_PARAM;
    }
    if (I2C_ESP32C3_OK != i2cEsp32c3CheckPort(self->port)) {
        return I2C_ESP32C3_ERR_PARAM;
    }
    if (I2C_ESP32C3_OK != i2cEsp32c3CheckAddress(deviceAddress)) {
        return I2C_ESP32C3_ERR_PARAM;
    }
    if (0U == self->installed) {
        return I2C_ESP32C3_ERR_PARAM;
    }

    cmd = i2c_cmd_link_create();
    if (NULL == cmd) {
        return I2C_ESP32C3_ERR_PARAM;
    }

    (void)i2c_master_start(cmd);
    (void)i2c_master_write_byte(cmd,
                                (uint8_t)((deviceAddress << 1) | I2C_MASTER_WRITE), true);
    (void)i2c_master_stop(cmd);

    err = i2c_master_cmd_begin((i2c_port_t)self->port, cmd,
                               i2cEsp32c3Ticks(I2C_ESP32C3_PROBE_TIMEOUT_MS));
    i2c_cmd_link_delete(cmd);

    return i2cEsp32c3MapError(err);
}

/* 运行时改时钟：IDF v5.5 legacy 头未提供 i2c_set_clock，
 * 统一用 i2c_param_config 重新下发 master.clk_speed（并同步 self->clockHz）。 */
static int i2cEsp32c3SetClock(void* instance, uint32_t clockHz)
{
    I2cEsp32c3* self     = (I2cEsp32c3*)instance;
    uint32_t    oldClock = 0;

    if (NULL == self) {
        return I2C_ESP32C3_ERR_PARAM;
    }
    if (0U == clockHz) {
        return I2C_ESP32C3_ERR_PARAM;
    }
    if (I2C_ESP32C3_OK != i2cEsp32c3CheckPort(self->port)) {
        return I2C_ESP32C3_ERR_PARAM;
    }

    oldClock = self->clockHz;
    if (I2C_ESP32C3_OK != i2cEsp32c3ApplyConfig(self, clockHz)) {
        /* 回滚到旧时钟，保持软描述与硬件一致 */
        (void)i2cEsp32c3ApplyConfig(self, oldClock);
        return I2C_ESP32C3_ERR_PARAM;
    }

    self->clockHz = clockHz;

    return I2C_ESP32C3_OK;
}

/* 复位 FIFO：硬件无独立复位，清零收发 FIFO 即可恢复正常收发 */
static int i2cEsp32c3Reset(void* instance)
{
    I2cEsp32c3* self = (I2cEsp32c3*)instance;
    esp_err_t    err = ESP_OK;

    if (NULL == self) {
        return I2C_ESP32C3_ERR_PARAM;
    }
    if (I2C_ESP32C3_OK != i2cEsp32c3CheckPort(self->port)) {
        return I2C_ESP32C3_ERR_PARAM;
    }
    if (0U == self->installed) {
        return I2C_ESP32C3_ERR_PARAM;
    }

    err = i2c_reset_tx_fifo((i2c_port_t)self->port);
    if (ESP_OK != err) {
        return I2C_ESP32C3_ERR_PARAM;
    }

    err = i2c_reset_rx_fifo((i2c_port_t)self->port);
    if (ESP_OK != err) {
        return I2C_ESP32C3_ERR_PARAM;
    }

    return I2C_ESP32C3_OK;
}

/* 具象析构：先卸载驱动（仅当已 install），再释放具象实例 */
static void i2cEsp32c3DeleteInstance(void* instance)
{
    I2cEsp32c3* self = (I2cEsp32c3*)instance;

    if (NULL == self) {
        return;
    }

    if ((0U != self->installed) &&
        (I2C_ESP32C3_OK == i2cEsp32c3CheckPort(self->port))) {
        (void)i2c_driver_delete((i2c_port_t)self->port);
        self->installed = 0;
    }

    if (NULL != i2cFree) {
        i2cFree(self);
    }
    self = NULL;
}

/* ==========================================================================
 * 接口表 + 具象构造
 * ========================================================================== */

I2cInterface I2cEsp32c3Interface(void)
{
    _RETURN_I2C_INTERFACE(i2cEsp32c3Init, i2cEsp32c3Deinit,
                          i2cEsp32c3Write, i2cEsp32c3Read,
                          i2cEsp32c3WriteRead, i2cEsp32c3Probe,
                          i2cEsp32c3SetClock, i2cEsp32c3Reset,
                          i2cEsp32c3DeleteInstance);
}

I2c* NewI2cEsp32c3(uint32_t port, uint32_t sdaPin, uint32_t sclPin, uint32_t clockHz)
{
    I2cEsp32c3* self   = NULL;
    I2c*        result = NULL;

    /* 参数校验：端口 / 引脚 / 时钟 */
    if (I2C_ESP32C3_OK != i2cEsp32c3CheckPort(port)) {
        return NULL;
    }
    if (sdaPin == sclPin) {
        return NULL; /* 同一引脚不可能同时作 SDA / SCL */
    }
    if (0U == clockHz) {
        return NULL;
    }
    if (NULL == i2cMalloc) {
        return NULL;
    }

    self = (I2cEsp32c3*)i2cMalloc(sizeof(I2cEsp32c3));
    if (NULL == self) {
        return NULL;
    }

    memset(self, 0, sizeof(I2cEsp32c3));
    self->port      = port;
    self->sdaPin    = sdaPin;
    self->sclPin    = sclPin;
    self->clockHz   = clockHz;
    self->installed = 0;

    /* 平台初始化失败：释放具象实例并返回 NULL */
    if (I2C_ESP32C3_OK != i2cEsp32c3Install(self)) {
        if (NULL != i2cFree) {
            i2cFree(self);
        }
        self = NULL;
        return NULL;
    }

    /* 抽象构造装配（禁止手工赋值 instance / interfaces） */
    result = NewI2c((void*)self, I2cEsp32c3Interface());
    if (NULL == result) {
        i2cEsp32c3DeleteInstance((void*)self); /* 卸载驱动 + 释放具象实例 */
        self = NULL;
        return NULL;
    }

    return result;
}
