#include "spi.h"
#include <stdlib.h>

/* ========== 内存分配器默认实现（可被外部整体替换）========== */
void* (*spiMalloc)(size_t size) = malloc;
void  (*spiFree)(void* p)       = free;

/* ========== 抽象类构造 ==========
 * 只做「分配抽象对象 + 存 instance + 存接口表」，不创建具体实例。
 */
Spi* NewSpi(void* instance, SpiInterface interfaces) {
    if (NULL == instance || NULL == interfaces.deleteInstance) return NULL;
    if (NULL == spiMalloc) return NULL;

    Spi* self = (Spi*)spiMalloc(sizeof(Spi));
    if (NULL == self) return NULL;

    self->instance   = instance;
    self->interfaces = interfaces;
    return self;
}

/* ========== 抽象类析构 ==========
 * ① 先调用具体实例的析构函数（销毁具体实现）
 * ② 再释放抽象对象自身
 * ③ 最后置空外部指针，防止悬空
 */
void DeleteSpi(Spi** pSpi) {
    if (NULL == pSpi || NULL == *pSpi) return;

    Spi* self = *pSpi;
    if (NULL != self->interfaces.deleteInstance && NULL != self->instance) {
        self->interfaces.deleteInstance(self->instance);
    }
    self->instance = NULL;

    if (NULL != spiFree) {
        spiFree(self);
    }
    *pSpi = NULL;
}

/* ========== 默认配置：mode0、8bit ========== */
SpiConfig SpiDefaultConfig(uint32_t clockHz) {
    SpiConfig config;
    config.clockHz     = clockHz;
    config.mode        = SpiMode0;
    config.bitsPerWord = 8;
    return config;
}

/* ========== 生命周期 ========== */
int SpiInit(Spi* spi) {
    if (NULL == spi || NULL == spi->instance) return -1;
    if (NULL == spi->interfaces.init) return -2;
    return spi->interfaces.init(spi->instance);
}

int SpiDeinit(Spi* spi) {
    if (NULL == spi || NULL == spi->instance) return -1;
    if (NULL == spi->interfaces.deinit) return -2;
    return spi->interfaces.deinit(spi->instance);
}

/* ========== 数据收发 ========== */
int SpiTransfer(Spi* spi, const uint8_t* txData, uint8_t* rxData, uint32_t len, uint32_t timeoutMs) {
    if (NULL == spi || NULL == spi->instance) return -1;
    /* 全双工：收发缓冲区不可同时为空 */
    if (NULL == txData && NULL == rxData) return -1;
    if (NULL == spi->interfaces.transfer) return -2;
    if (0u == len) return 0;

    return spi->interfaces.transfer(spi->instance, txData, rxData, len, timeoutMs);
}

int SpiWrite(Spi* spi, const uint8_t* txData, uint32_t len, uint32_t timeoutMs) {
    if (NULL == spi || NULL == spi->instance) return -1;
    if (NULL == txData) return -1;
    if (NULL == spi->interfaces.write) return -2;
    if (0u == len) return 0;

    return spi->interfaces.write(spi->instance, txData, len, timeoutMs);
}

int SpiRead(Spi* spi, uint8_t* rxData, uint32_t len, uint32_t timeoutMs) {
    if (NULL == spi || NULL == spi->instance) return -1;
    if (NULL == rxData) return -1;
    if (NULL == spi->interfaces.read) return -2;
    if (0u == len) return 0;

    return spi->interfaces.read(spi->instance, rxData, len, timeoutMs);
}

/* ========== 片选：active 非 0 = 拉低选中 ========== */
int SpiSelect(Spi* spi, int active) {
    if (NULL == spi || NULL == spi->instance) return -1;
    if (NULL == spi->interfaces.select) return -2;

    return spi->interfaces.select(spi->instance, (0 != active) ? 1 : 0);
}

/* ========== 运行期重配置 ========== */
int SpiSetConfig(Spi* spi, const SpiConfig* config) {
    if (NULL == spi || NULL == spi->instance) return -1;
    if (NULL == config || 0u == config->bitsPerWord) return -1;
    if (NULL == spi->interfaces.setConfig) return -2;

    return spi->interfaces.setConfig(spi->instance, config);
}
