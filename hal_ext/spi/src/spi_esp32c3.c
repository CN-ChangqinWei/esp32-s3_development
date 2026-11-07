/**
 * @file spi_esp32c3.c
 * @brief SPI 具象派生类实现：ESP32-C3（ESP-IDF v5.5.x，仅 SPI2_HOST）。
 *
 * 职责边界：
 *   ① NewSpiEsp32c3()：校验入参 → 分配 SpiEsp32c3 → 初始化总线/设备 → 调用 NewSpi()
 *   ② 接口函数全部 static，首参恒为 void* instance，首行取具体实例
 *   ③ 片选由本层手动控制（devcfg.spics_io_num = -1），保证 SpiSelect() 语义可信
 *   ④ 内存一律走 spiMalloc / spiFree（可在外部整体替换）
 *
 * 平台类型（spi_device_handle_t 等）只出现在本文件，不外泄到头文件。
 */

#include "spi_esp32c3.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_log.h"

#include <string.h>

static const char* TAG = "SPI_C3";

/* ------------------------------------------------------------------
 * 内部常量
 * ------------------------------------------------------------------ */
#define SPI_ESP32C3_HOST                SPI2_HOST        /* ESP32-C3 仅通用 SPI2 */
#define SPI_ESP32C3_DEFAULT_CLOCK_HZ    (1000000u)       /* config == NULL 时使用 */
#define SPI_ESP32C3_MAX_TRANSFER_BYTES  (4096)           /* 单次传输上限（字节） */
#define SPI_ESP32C3_QUEUE_SIZE          (1)              /* polling 传输，队列 1 足够 */
#define SPI_ESP32C3_DEFAULT_BITS        (8u)             /* bitsPerWord 归一值 */
#define SPI_ESP32C3_PIN_NONE            (0xFFFFFFFFu)    /* 入参 -1 的 uint32_t 形式 */
#define SPI_ESP32C3_PIN_MAX             (48u)            /* 通用 GPIO 上界（C3 实际 0~21） */

/* ==================================================================
 * 内部工具
 * ================================================================== */

/* 入参引脚 uint32_t → int32_t：0xFFFFFFFF 约定为「未使用」，归一化成 -1 */
static int32_t spiEsp32c3NormalizePin(uint32_t pin) {
    return (SPI_ESP32C3_PIN_NONE == pin) ? -1 : (int32_t)pin;
}

/* 引脚合法性：允许「未使用」，否则必须落在合法 GPIO 号范围内 */
static int spiEsp32c3CheckPin(uint32_t pin) {
    if (SPI_ESP32C3_PIN_NONE == pin) {
        return SPI_ESP32C3_OK;
    }
    return (pin <= SPI_ESP32C3_PIN_MAX) ? SPI_ESP32C3_OK : SPI_ESP32C3_ERR_PARAM;
}

/* 由「时钟 / 模式」构造设备配置
 * 关键：spics_io_num 固定 -1 —— 交给驱动会在每次传输自动拉 CS，
 *      与抽象层 select() 的显式语义冲突，故片选一律由具象层手动控制。
 * 其余字段 memset 归零，由驱动取默认值（占用率 50%、无 pre/post 回调等）。
 */
static void spiEsp32c3BuildDeviceConfig(uint32_t clockHz, uint8_t mode,
                                        spi_device_interface_config_t* devcfg) {
    memset(devcfg, 0, sizeof(*devcfg));

    devcfg->clock_speed_hz = (int)clockHz;
    devcfg->mode           = (int)mode;   /* SpiMode0..3 与 IDF mode 0..3 一一对应 */
    devcfg->spics_io_num   = -1;          /* 片选不交给驱动 */
    devcfg->queue_size     = SPI_ESP32C3_QUEUE_SIZE;
}

/* 建立硬件：片选 GPIO → 总线 → 设备
 * 幂等：已就绪的部分自动跳过，因此 init() 与 NewXxx() 可共用本函数。
 */
static int spiEsp32c3Setup(SpiEsp32c3* self) {
    if (NULL == self) {
        return SPI_ESP32C3_ERR_PARAM;
    }

    /* ① 片选引脚：配置为输出并置高（未选中），避免上电瞬间误选从机 */
    if (self->csPin >= 0 && 0u == self->csReady) {
        if (ESP_OK != gpio_set_direction((gpio_num_t)self->csPin, GPIO_MODE_OUTPUT)) {
            ESP_LOGE(TAG, "cs gpio %ld set direction failed", (long)self->csPin);
            return SPI_ESP32C3_ERR_PARAM;
        }
        (void)gpio_set_level((gpio_num_t)self->csPin, 1);
        self->csReady = 1u;
    }

    /* ② 总线初始化（DMA 自动分配通道） */
    if (0u == self->busInitialized) {
        spi_bus_config_t buscfg;
        esp_err_t err = ESP_OK;

        memset(&buscfg, 0, sizeof(buscfg));
        buscfg.mosi_io_num     = (int)self->mosiPin;
        buscfg.miso_io_num     = (int)self->misoPin;
        buscfg.sclk_io_num     = (int)self->sclkPin;
        buscfg.quadwp_io_num   = -1;
        buscfg.quadhd_io_num   = -1;
        buscfg.max_transfer_sz = SPI_ESP32C3_MAX_TRANSFER_BYTES;

        err = spi_bus_initialize(SPI_ESP32C3_HOST, &buscfg, SPI_DMA_CH_AUTO);
        if (ESP_OK != err) {
            ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
            return SPI_ESP32C3_ERR_PARAM;
        }
        self->busInitialized = 1u;
    }

    /* ③ 挂载设备（片选已排除在驱动之外） */
    if (NULL == self->deviceHandle) {
        spi_device_interface_config_t devcfg;
        spi_device_handle_t handle = NULL;
        esp_err_t err = ESP_OK;

        spiEsp32c3BuildDeviceConfig(self->clockHz, self->mode, &devcfg);
        err = spi_bus_add_device(SPI_ESP32C3_HOST, &devcfg, &handle);
        if (ESP_OK != err) {
            ESP_LOGE(TAG, "spi_bus_add_device failed: %s", esp_err_to_name(err));
            return SPI_ESP32C3_ERR_PARAM;
        }
        self->deviceHandle = (void*)handle;
    }

    return SPI_ESP32C3_OK;
}

/* 拆除硬件：设备 → 总线
 * 空指针安全、可重复调用；仅在平台调用成功时清除状态标志，
 * 失败时保留标志以便上层 / 析构阶段二次处理，不会重复释放同一句柄。
 */
static int spiEsp32c3Teardown(SpiEsp32c3* self) {
    int result = SPI_ESP32C3_OK;

    if (NULL == self) {
        return SPI_ESP32C3_ERR_PARAM;
    }

    if (NULL != self->deviceHandle) {
        esp_err_t err = spi_bus_remove_device((spi_device_handle_t)self->deviceHandle);
        if (ESP_OK != err) {
            ESP_LOGE(TAG, "spi_bus_remove_device failed: %s", esp_err_to_name(err));
            result = SPI_ESP32C3_ERR_PARAM;
        } else {
            self->deviceHandle = NULL;
        }
    }

    if (0u != self->busInitialized) {
        esp_err_t err = spi_bus_free(SPI_ESP32C3_HOST);
        if (ESP_OK != err) {
            ESP_LOGE(TAG, "spi_bus_free failed: %s", esp_err_to_name(err));
            result = SPI_ESP32C3_ERR_PARAM;
        } else {
            self->busInitialized = 0u;
        }
    }

    return result;
}

/* ==================================================================
 * 虚表实现（全部 static，首参恒为 void* instance）
 * ================================================================== */

/* 生命周期：构造期已完成装配，此处幂等；deinit 之后可再次 init 复活 */
static int spiEsp32c3Init(void* instance) {
    SpiEsp32c3* self = (SpiEsp32c3*)instance;

    if (NULL == self) {
        return SPI_ESP32C3_ERR_PARAM;
    }

    return spiEsp32c3Setup(self);
}

static int spiEsp32c3Deinit(void* instance) {
    SpiEsp32c3* self = (SpiEsp32c3*)instance;

    if (NULL == self) {
        return SPI_ESP32C3_ERR_PARAM;
    }

    return spiEsp32c3Teardown(self);
}

/* 全双工传输：txData / rxData 允许其一为 NULL（单向收发），但不可同时为 NULL */
static int spiEsp32c3Transfer(void* instance, const uint8_t* txData, uint8_t* rxData,
                              uint32_t len, uint32_t timeoutMs) {
    SpiEsp32c3* self = (SpiEsp32c3*)instance;
    spi_transaction_t trans;
    esp_err_t err = ESP_OK;

    if (NULL == self || NULL == self->deviceHandle) {
        return SPI_ESP32C3_ERR_PARAM;
    }
    if (NULL == txData && NULL == rxData) {
        return SPI_ESP32C3_ERR_PARAM;
    }
    if (0u == len) {
        return 0;
    }

    /* polling 传输本身不可超时，timeoutMs 仅作语义保留：
     * 上层可据此判断"本层并未真正阻塞等待"，无需额外告警。
     */
    (void)timeoutMs;

    memset(&trans, 0, sizeof(trans));
    trans.length    = (size_t)len * 8u;   /* 驱动以 bit 计长 */
    trans.tx_buffer = txData;             /* NULL = 只收不发 */
    trans.rx_buffer = rxData;             /* NULL = 只发不收 */

    err = spi_device_polling_transmit((spi_device_handle_t)self->deviceHandle, &trans);
    if (ESP_OK != err) {
        ESP_LOGE(TAG, "polling transmit failed: %s", esp_err_to_name(err));
        return SPI_ESP32C3_ERR_PARAM;
    }

    return (int)len;
}

/* 只写：rx_buffer = NULL */
static int spiEsp32c3Write(void* instance, const uint8_t* txData, uint32_t len,
                           uint32_t timeoutMs) {
    if (NULL == instance || NULL == txData) {
        return SPI_ESP32C3_ERR_PARAM;
    }

    return spiEsp32c3Transfer(instance, txData, NULL, len, timeoutMs);
}

/* 只读：tx_buffer = NULL */
static int spiEsp32c3Read(void* instance, uint8_t* rxData, uint32_t len,
                          uint32_t timeoutMs) {
    if (NULL == instance || NULL == rxData) {
        return SPI_ESP32C3_ERR_PARAM;
    }

    return spiEsp32c3Transfer(instance, NULL, rxData, len, timeoutMs);
}

/* 片选：active 非 0 → 拉低选中；无片选引脚时返回 -2 */
static int spiEsp32c3Select(void* instance, int active) {
    SpiEsp32c3* self = (SpiEsp32c3*)instance;

    if (NULL == self) {
        return SPI_ESP32C3_ERR_PARAM;
    }
    if (self->csPin < 0) {
        return SPI_ESP32C3_ERR_NO_CS;
    }
    if (0u == self->csReady) {
        ESP_LOGW(TAG, "cs gpio %ld not ready, call SpiInit first", (long)self->csPin);
        return SPI_ESP32C3_ERR_PARAM;
    }

    if (ESP_OK != gpio_set_level((gpio_num_t)self->csPin, (0 != active) ? 0 : 1)) {
        ESP_LOGE(TAG, "cs gpio %ld set level failed", (long)self->csPin);
        return SPI_ESP32C3_ERR_PARAM;
    }

    return SPI_ESP32C3_OK;
}

/* 运行期重配置：摘除旧设备 → 按新配置重挂 → 更新本地配置副本 */
static int spiEsp32c3SetConfig(void* instance, const SpiConfig* config) {
    SpiEsp32c3* self = (SpiEsp32c3*)instance;
    spi_device_interface_config_t devcfg;
    spi_device_handle_t handle = NULL;
    uint32_t clockHz = 0u;
    uint8_t  mode = 0u;
    uint8_t  bitsPerWord = 0u;
    esp_err_t err = ESP_OK;

    if (NULL == self || NULL == config) {
        return SPI_ESP32C3_ERR_PARAM;
    }
    if (0u == config->clockHz || config->mode < SpiMode0 || config->mode > SpiMode3) {
        ESP_LOGE(TAG, "invalid config: clockHz=%lu mode=%d",
                 (unsigned long)config->clockHz, (int)config->mode);
        return SPI_ESP32C3_ERR_PARAM;
    }

    clockHz     = config->clockHz;
    mode        = (uint8_t)config->mode;
    bitsPerWord = (0u == config->bitsPerWord) ? SPI_ESP32C3_DEFAULT_BITS : config->bitsPerWord;

    /* 设备尚未挂载（从未 init 或已 deinit）：只记录配置，下次 init 生效 */
    if (NULL == self->deviceHandle) {
        self->clockHz     = clockHz;
        self->mode        = mode;
        self->bitsPerWord = bitsPerWord;
        return SPI_ESP32C3_OK;
    }

    /* ① 摘除旧设备 */
    err = spi_bus_remove_device((spi_device_handle_t)self->deviceHandle);
    if (ESP_OK != err) {
        ESP_LOGE(TAG, "spi_bus_remove_device failed: %s", esp_err_to_name(err));
        return SPI_ESP32C3_ERR_PARAM;
    }
    self->deviceHandle = NULL;

    /* ② 以新配置重挂设备 */
    spiEsp32c3BuildDeviceConfig(clockHz, mode, &devcfg);
    err = spi_bus_add_device(SPI_ESP32C3_HOST, &devcfg, &handle);
    if (ESP_OK != err) {
        ESP_LOGE(TAG, "spi_bus_add_device with updated cfg failed: %s, restoring old cfg",
                 esp_err_to_name(err));

        /* ③ 回滚：用旧配置尽量恢复设备，避免 SPI 通道彻底不可用
         *    此时 self->clockHz / self->mode 仍是旧值，可直接复用
         */
        handle = NULL;
        spiEsp32c3BuildDeviceConfig(self->clockHz, self->mode, &devcfg);
        if (ESP_OK == spi_bus_add_device(SPI_ESP32C3_HOST, &devcfg, &handle)) {
            self->deviceHandle = (void*)handle;
        } else {
            ESP_LOGE(TAG, "restore device failed, spi unusable until init");
        }
        return SPI_ESP32C3_ERR_PARAM;
    }
    self->deviceHandle = (void*)handle;

    /* ④ 更新本地配置副本（仅在新配置真正生效后） */
    self->clockHz     = clockHz;
    self->mode        = mode;
    self->bitsPerWord = bitsPerWord;

    return SPI_ESP32C3_OK;
}

/* 析构：移除设备 + 释放总线 → 释放具体实例
 * 空指针安全；部分初始化失败时依靠状态标志避免重复释放。
 */
static void spiEsp32c3DeleteInstance(void* instance) {
    SpiEsp32c3* self = (SpiEsp32c3*)instance;

    if (NULL == self) {
        return;
    }

    (void)spiEsp32c3Teardown(self);

    /* 片选保持高电平（未选中），防止设备意外被选中 */
    if (self->csPin >= 0 && 0u != self->csReady) {
        (void)gpio_set_level((gpio_num_t)self->csPin, 1);
        self->csReady = 0u;
    }

    if (NULL != spiFree) {
        spiFree(self);
    }
    /* self 是局部副本，置空无意义；调用方持有的是 Spi 抽象对象 */
}

/* ==================================================================
 * 虚表集中返回：字段顺序严格遵循 SpiInterface
 * ================================================================== */
SpiInterface SpiEsp32c3Interface(void) {
    _RETURN_SPI_INTERFACE_BY(spiEsp32c3Init,
                             spiEsp32c3Deinit,
                             spiEsp32c3Transfer,
                             spiEsp32c3Write,
                             spiEsp32c3Read,
                             spiEsp32c3Select,
                             spiEsp32c3SetConfig,
                             spiEsp32c3DeleteInstance);
}

/* ==================================================================
 * 具体构造
 * ================================================================== */
Spi* NewSpiEsp32c3(uint32_t sclkPin, uint32_t mosiPin, uint32_t misoPin,
                   uint32_t csPin, const SpiConfig* config) {
    SpiEsp32c3* ins = NULL;
    Spi* spi = NULL;

    /* ---------- ① 入参校验 ---------- */
    if (SPI_ESP32C3_PIN_NONE == sclkPin) {
        ESP_LOGE(TAG, "sclk pin is required");
        return NULL;
    }
    if (SPI_ESP32C3_OK != spiEsp32c3CheckPin(sclkPin) ||
        SPI_ESP32C3_OK != spiEsp32c3CheckPin(mosiPin) ||
        SPI_ESP32C3_OK != spiEsp32c3CheckPin(misoPin) ||
        SPI_ESP32C3_OK != spiEsp32c3CheckPin(csPin)) {
        ESP_LOGE(TAG, "invalid pin number");
        return NULL;
    }
    if (NULL == spiMalloc || NULL == spiFree) {
        ESP_LOGE(TAG, "memory hooks not ready");
        return NULL;
    }

    /* ---------- ② 分配并填充具体实例 ---------- */
    ins = (SpiEsp32c3*)spiMalloc(sizeof(SpiEsp32c3));
    if (NULL == ins) {
        ESP_LOGE(TAG, "no memory for SpiEsp32c3");
        return NULL;
    }
    /* memset 已把 deviceHandle / busInitialized / csReady 清零 */
    memset(ins, 0, sizeof(*ins));

    ins->sclkPin = spiEsp32c3NormalizePin(sclkPin);
    ins->mosiPin = spiEsp32c3NormalizePin(mosiPin);
    ins->misoPin = spiEsp32c3NormalizePin(misoPin);
    ins->csPin   = spiEsp32c3NormalizePin(csPin);

    if (NULL == config) {
        SpiConfig defaultConfig = SpiDefaultConfig(SPI_ESP32C3_DEFAULT_CLOCK_HZ);

        ins->clockHz     = defaultConfig.clockHz;
        ins->mode        = (uint8_t)defaultConfig.mode;
        ins->bitsPerWord = defaultConfig.bitsPerWord;
    } else {
        ins->clockHz     = (0u == config->clockHz) ? SPI_ESP32C3_DEFAULT_CLOCK_HZ : config->clockHz;
        ins->mode        = (config->mode >= SpiMode0 && config->mode <= SpiMode3)
                           ? (uint8_t)config->mode : (uint8_t)SpiMode0;
        ins->bitsPerWord = (0u == config->bitsPerWord) ? SPI_ESP32C3_DEFAULT_BITS
                                                       : config->bitsPerWord;
    }

    /* ---------- ③ 建立总线与设备（失败即回滚） ---------- */
    if (SPI_ESP32C3_OK != spiEsp32c3Setup(ins)) {
        ESP_LOGE(TAG, "esp32c3 spi setup failed, rollback");
        (void)spiEsp32c3Teardown(ins);
        spiFree(ins);
        return NULL;
    }

    /* ---------- ④ 装配抽象父类：禁止手工赋值 instance / interfaces ---------- */
    spi = NewSpi((void*)ins, SpiEsp32c3Interface());
    if (NULL == spi) {
        ESP_LOGE(TAG, "NewSpi failed, rollback");
        (void)spiEsp32c3Teardown(ins);
        spiFree(ins);
        return NULL;
    }

    return spi;
}
