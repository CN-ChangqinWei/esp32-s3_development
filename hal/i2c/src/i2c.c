#include "i2c.h"

#include <stdlib.h>

/* ==========================================================================
 * I2C 抽象父类实现（纯转发层，零平台依赖）
 *
 * 本文件只负责：
 *   1. 提供默认内存分配钩子（malloc / free）；
 *   2. 抽象构造 NewI2c / 析构 DeleteI2c；
 *   3. 对外包装函数的入参校验与虚表转发。
 * 具体总线操作全部由具象派生类通过 I2cInterface 注入。
 * ========================================================================== */

/* 默认内存钩子，可被具象模块或应用层整体替换 */
void* (*i2cMalloc)(size_t size) = malloc;
void  (*i2cFree)(void* p)       = free;

/* 句柄基本校验：i2c 本身与外部注入的具象实例都必须非空 */
static int i2cCheckHandle(const I2c* self)
{
    if (NULL == self) {
        return -1;
    }
    if (NULL == self->instance) {
        return -1;
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * 抽象构造：分配抽象对象 + 存 instance + 存 interfaces，不创建具象实例
 * -------------------------------------------------------------------------- */
I2c* NewI2c(void* instance, I2cInterface interfaces)
{
    I2c* self = NULL;

    if (NULL == instance || NULL == interfaces.deleteInstance) {
        return NULL;
    }

    self = (I2c*)i2cMalloc(sizeof(I2c));
    if (NULL == self) {
        return NULL;
    }

    self->instance   = instance;
    self->interfaces = interfaces;

    return self;
}

/* --------------------------------------------------------------------------
 * 析构：先销毁具象实例，再释放抽象对象，最后把调用者的指针置空
 * -------------------------------------------------------------------------- */
void DeleteI2c(I2c** pI2c)
{
    I2c* self = NULL;

    if (NULL == pI2c) {
        return;
    }

    self = *pI2c;
    if (NULL == self) {
        return;
    }

    if (NULL != self->interfaces.deleteInstance) {
        self->interfaces.deleteInstance(self->instance);
    }

    self->instance   = NULL;
    self->interfaces.deleteInstance = NULL;

    i2cFree(self);

    *pI2c = NULL;
}

/* --------------------------------------------------------------------------
 * 生命周期
 * -------------------------------------------------------------------------- */
int I2cInit(I2c* i2c)
{
    int rc = i2cCheckHandle(i2c);

    if (0 != rc) {
        return rc;
    }
    if (NULL == i2c->interfaces.init) {
        return -2;
    }

    return i2c->interfaces.init(i2c->instance);
}

int I2cDeinit(I2c* i2c)
{
    int rc = i2cCheckHandle(i2c);

    if (0 != rc) {
        return rc;
    }
    if (NULL == i2c->interfaces.deinit) {
        return -2;
    }

    return i2c->interfaces.deinit(i2c->instance);
}

int I2cReset(I2c* i2c)
{
    int rc = i2cCheckHandle(i2c);

    if (0 != rc) {
        return rc;
    }
    if (NULL == i2c->interfaces.reset) {
        return -2;
    }

    return i2c->interfaces.reset(i2c->instance);
}

/* --------------------------------------------------------------------------
 * 配置
 * -------------------------------------------------------------------------- */
int I2cSetClock(I2c* i2c, uint32_t clockHz)
{
    int rc = i2cCheckHandle(i2c);

    if (0 != rc) {
        return rc;
    }
    if (0 == clockHz) {
        return -1;
    }
    if (NULL == i2c->interfaces.setClock) {
        return -2;
    }

    return i2c->interfaces.setClock(i2c->instance, clockHz);
}

/* --------------------------------------------------------------------------
 * 数据收发
 * -------------------------------------------------------------------------- */
int I2cWrite(I2c* i2c, uint16_t deviceAddress, const uint8_t* data, uint32_t len, uint32_t timeoutMs)
{
    int rc = i2cCheckHandle(i2c);

    if (0 != rc) {
        return rc;
    }
    if ((NULL == data) || (0 == len)) {
        return -1;
    }
    if (NULL == i2c->interfaces.write) {
        return -2;
    }

    return i2c->interfaces.write(i2c->instance, deviceAddress, data, len, timeoutMs);
}

int I2cRead(I2c* i2c, uint16_t deviceAddress, uint8_t* buffer, uint32_t len, uint32_t timeoutMs)
{
    int rc = i2cCheckHandle(i2c);

    if (0 != rc) {
        return rc;
    }
    if ((NULL == buffer) || (0 == len)) {
        return -1;
    }
    if (NULL == i2c->interfaces.read) {
        return -2;
    }

    return i2c->interfaces.read(i2c->instance, deviceAddress, buffer, len, timeoutMs);
}

int I2cWriteRead(I2c* i2c, uint16_t deviceAddress,
                 const uint8_t* writeData, uint32_t writeLen,
                 uint8_t* readBuffer, uint32_t readLen, uint32_t timeoutMs)
{
    int rc = i2cCheckHandle(i2c);

    if (0 != rc) {
        return rc;
    }
    /* 双向长度至少一侧有效 */
    if ((0 == writeLen) && (0 == readLen)) {
        return -1;
    }
    if ((writeLen > 0) && (NULL == writeData)) {
        return -1;
    }
    if ((readLen > 0) && (NULL == readBuffer)) {
        return -1;
    }
    if (NULL == i2c->interfaces.writeRead) {
        return -2;
    }

    return i2c->interfaces.writeRead(i2c->instance, deviceAddress,
                                     writeData, writeLen,
                                     readBuffer, readLen, timeoutMs);
}

/* --------------------------------------------------------------------------
 * 设备探测：0 表示在线，-3 表示无应答，-1/-2 见对外返回值约定
 * -------------------------------------------------------------------------- */
int I2cProbe(I2c* i2c, uint16_t deviceAddress)
{
    int rc = i2cCheckHandle(i2c);

    if (0 != rc) {
        return rc;
    }
    if (NULL == i2c->interfaces.probe) {
        return -2;
    }

    return i2c->interfaces.probe(i2c->instance, deviceAddress);
}
