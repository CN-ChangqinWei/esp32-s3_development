/**
 * @file sc7a20.c
 * @brief SC7A20HTR 三轴数字加速度计驱动实现（走抽象 I2C，装配时注入）。
 *
 * 本文件保持平台无关：只 include 标准头与抽象 i2c.h，
 * 不出现任何 ESP-IDF / FreeRTOS 头文件，所有总线访问一律通过 I2c 虚表完成。
 *
 * 器件兼容性说明（待实测确认）：
 *   板上 U8 为 SC7A20HTR，网表未附数据手册，寄存器布局按 LIS2DH / LIS2DH12
 *   系列兼容实现。若实测 WHO_AM_I 或输出换算不符，需按实测数据手册复核
 *   本文件顶部的寄存器地址与灵敏度表。
 *
 * 返回值约定（与 sc7a20.h 一致）：
 *    0  成功
 *   -1  参数非法
 *   -2  未初始化 / 未实现
 *   -3  器件无应答 / 身份不符
 */

#include "sc7a20.h"
#include "i2c.h"

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* ==========================================================================
 * 一、返回值约定
 * ========================================================================== */
#define SC7A20_OK              0
#define SC7A20_ERR_PARAM       (-1)
#define SC7A20_ERR_NOT_READY   (-2)
#define SC7A20_ERR_NO_ACK      (-3)

/* ==========================================================================
 * 二、板级常量
 * ========================================================================== */
/* I2C 事务超时：器件为 400kHz 低速外设，50ms 足以覆盖单字节读写 */
#define SC7A20_TIMEOUT_MS      50u

/* 7bit 地址上限：0x7F（0x00 为广播地址，对器件驱动无意义，构造时拒绝） */
#define SC7A20_ADDRESS7_MAX    0x7Fu

/* ==========================================================================
 * 三、寄存器映射（LIS2DH / LIS2DH12 兼容布局，待实测确认）
 * ========================================================================== */
#define SC7A20_REG_WHO_AM_I    0x0Fu   /* 器件标识，只读 */
#define SC7A20_REG_CTRL_REG1   0x20u   /* ODR[7:4] + LPen(bit3) + XYZ enable[2:0] */
#define SC7A20_REG_CTRL_REG4   0x23u   /* BDU(bit7) + FS[5:4] + HR(bit3) */
#define SC7A20_REG_OUT_X_L     0x28u   /* 输出起始地址，须配合自增位多字节读 */

/* 多字节读时子地址最高位置 1，令器件内部地址指针自动递增 */
#define SC7A20_AUTO_INCREMENT  0x80u

/* CTRL_REG1 位定义 */
#define SC7A20_CTRL1_XYZ_ENABLE  0x07u  /* X / Y / Z 三轴使能 0b111 */

/* CTRL_REG4 位定义 */
#define SC7A20_CTRL4_BDU         0x80u  /* 块数据更新：读期间冻结高低字节，防撕裂 */
#define SC7A20_CTRL4_HR          0x08u  /* 高分辨率模式：输出 12bit（换算按 12bit） */

/* 输出缓冲：X/Y/Z 各 2 字节小端 */
#define SC7A20_OUTPUT_BYTES      6u
/* 12bit 左对齐值需要右移掉的填充位数 */
#define SC7A20_LEFT_ALIGN_SHIFT  4

/* ==========================================================================
 * 四、内存分配钩子默认实现（外部可在创建对象之前整体替换）
 * ========================================================================== */
void* (*sc7a20Malloc)(size_t size) = malloc;
void  (*sc7a20Free)(void* p)       = free;

/* ==========================================================================
 * 五、内部工具
 * ========================================================================== */

/* 句柄基本校验：device 本身与注入的抽象总线都必须非空 */
static int sc7a20CheckHandle(const Sc7a20* device)
{
    if (NULL == device) {
        return SC7A20_ERR_PARAM;
    }
    if (NULL == device->bus) {
        return SC7A20_ERR_PARAM;
    }
    return SC7A20_OK;
}

/* 总线返回值归一：I2C 抽象层返回 0 为成功，任何负值统一视为器件无应答 */
static int sc7a20MapBusResult(int busResult)
{
    return (busResult < 0) ? SC7A20_ERR_NO_ACK : SC7A20_OK;
}

/* 写单个寄存器：把「寄存器地址 + 数据」拼成 2 字节一次发出，
 * 先发寄存器地址再发数据，符合器件单寄存器写时序（无需自增位）。*/
static int sc7a20WriteRegister(Sc7a20* self, uint8_t reg, uint8_t value)
{
    uint8_t buffer[2];

    buffer[0] = reg;
    buffer[1] = value;

    return sc7a20MapBusResult(
        I2cWrite(self->bus, self->address7, buffer, 2u, SC7A20_TIMEOUT_MS));
}

/* 读寄存器（组）：先写子地址再读数据。
 * subAddress 最高位为 1 时器件内部地址指针在多字节读过程中自动递增。*/
static int sc7a20ReadRegisterGroup(Sc7a20* self, uint8_t subAddress,
                                   uint8_t* buffer, uint32_t len)
{
    return sc7a20MapBusResult(
        I2cWriteRead(self->bus, self->address7, &subAddress, 1u,
                     buffer, len, SC7A20_TIMEOUT_MS));
}

/* odrHz 转为 CTRL_REG1 的 ODR[7:4] 编码；返回 0 表示输入非法
 * （0x0 对应掉电模式，本驱动不提供该档）。
 * 形参必须为 uint16_t：400Hz 档超出 uint8_t 上限(255)，8 位会被截断成 144
 * 从而落入 default 返回 0，使 400Hz 档永久无法命中。*/
static uint8_t sc7a20OdrCode(uint16_t odrHz)
{
    switch (odrHz) {
        case 1u:   return 0x1u;   /* 1 Hz */
        case 10u:  return 0x2u;   /* 10 Hz */
        case 25u:  return 0x3u;   /* 25 Hz */
        case 50u:  return 0x4u;   /* 50 Hz */
        case 100u: return 0x5u;   /* 100 Hz */
        case 200u: return 0x6u;   /* 200 Hz */
        case 400u: return 0x7u;   /* 400 Hz */
        default:   return 0x0u;   /* 非法标记 */
    }
}

/* rangeG 转为 CTRL_REG4 的 FS[5:4] 编码；返回 0xFF 表示输入非法 */
static uint8_t sc7a20RangeBits(uint8_t rangeG)
{
    switch (rangeG) {
        case 2u:  return 0x0u;    /* FS = 00，正负 2g */
        case 4u:  return 0x1u;    /* FS = 01，正负 4g */
        case 8u:  return 0x2u;    /* FS = 10，正负 8g */
        case 16u: return 0x3u;    /* FS = 11，正负 16g */
        default:  return 0xFFu;   /* 非法标记 */
    }
}

/* 灵敏度表：针对 12bit 输出值，单位 mg/LSB；返回 0 表示量程非法 */
static int32_t sc7a20SensitivityMgPerLsb(uint8_t rangeG)
{
    switch (rangeG) {
        case 2u:  return 1;
        case 4u:  return 2;
        case 8u:  return 4;
        case 16u: return 12;
        default:  return 0;
    }
}

/* 两字节小端拼成有符号 16bit，再算术右移 4 位得到左对齐的 12bit 值 */
static int16_t sc7a20ToSigned12(const uint8_t* buffer)
{
    int32_t value = 0;

    value = (int32_t)(((uint16_t)buffer[1] << 8) | (uint16_t)buffer[0]);
    return (int16_t)(value >> SC7A20_LEFT_ALIGN_SHIFT);
}

/* ==========================================================================
 * 六、构造 / 析构
 * ========================================================================== */

/* 具体构造：分配实例、校验配置、落地字段。构造阶段不访问总线。*/
Sc7a20* NewSc7a20(const Sc7a20Config* config)
{
    Sc7a20* self = NULL;

    if (NULL == sc7a20Malloc) {
        return NULL;
    }

    self = (Sc7a20*)sc7a20Malloc(sizeof(Sc7a20));
    if (NULL == self) {
        return NULL;
    }
    (void)memset(self, 0, sizeof(Sc7a20));

    /* 配置校验：任一项不合法则回滚，绝不返回半成品对象 */
    if (NULL == config) {
        goto rollback;
    }
    if (NULL == config->bus) {
        goto rollback;
    }
    if ((0u == config->address7) || (config->address7 > SC7A20_ADDRESS7_MAX)) {
        goto rollback;
    }
    if (0xFFu == sc7a20RangeBits(config->rangeG)) {
        goto rollback;
    }
    if (0x0u == sc7a20OdrCode(config->odrHz)) {
        goto rollback;
    }

    self->bus            = config->bus;
    self->address7       = config->address7;
    self->expectedWhoAmI = config->expectedWhoAmI;
    self->rangeG         = config->rangeG;
    self->odrHz          = config->odrHz;
    self->initialized    = 0u;

    return self;

rollback:
    if (NULL != sc7a20Free) {
        sc7a20Free(self);
    }
    self = NULL;
    return NULL;
}

/* 析构：只释放自身；抽象总线由装配方持有，此处仅解除引用 */
void DeleteSc7a20(Sc7a20** pDevice)
{
    Sc7a20* self = NULL;

    if (NULL == pDevice) {
        return;
    }

    self = *pDevice;
    if (NULL == self) {
        return;
    }

    self->bus         = NULL;
    self->initialized = 0u;

    if (NULL != sc7a20Free) {
        sc7a20Free(self);
    }
    self = NULL;

    /* 外部指针置空，防悬空 */
    *pDevice = NULL;
}

/* ==========================================================================
 * 七、运行期操作
 * ========================================================================== */

/* 读器件标识：不需先 Init（Init 自身依赖它做身份校验）*/
int Sc7a20ReadWhoAmI(Sc7a20* device, uint8_t* whoAmI)
{
    int rc = sc7a20CheckHandle(device);

    if (SC7A20_OK != rc) {
        return rc;
    }
    if (NULL == whoAmI) {
        return SC7A20_ERR_PARAM;
    }

    *whoAmI = 0u;

    /* 单字节读，不需要自增位 */
    return sc7a20ReadRegisterGroup(device, SC7A20_REG_WHO_AM_I, whoAmI, 1u);
}

int Sc7a20Init(Sc7a20* device)
{
    int rc = SC7A20_OK;
    uint8_t whoAmI  = 0u;
    uint8_t ctrlReg1 = 0u;
    uint8_t ctrlReg4 = 0u;

    rc = sc7a20CheckHandle(device);
    if (SC7A20_OK != rc) {
        return rc;
    }

    /* 幂等：每次调用都重新走完整流程，器件意外复位后同样能恢复配置 */
    device->initialized = 0u;

    /* ① 身份校验：expectedWhoAmI 为 0 表示跳过校验 */
    rc = Sc7a20ReadWhoAmI(device, &whoAmI);
    if (SC7A20_OK != rc) {
        return rc;
    }
    if ((0u != device->expectedWhoAmI) && (whoAmI != device->expectedWhoAmI)) {
        return SC7A20_ERR_NO_ACK;
    }

    /* ② CTRL_REG1：ODR[7:4] + LPen(bit3)=0 保持普通模式 + XYZ 使能 0b111 */
    ctrlReg1 = (uint8_t)((uint8_t)(sc7a20OdrCode(device->odrHz) << 4)
                         | SC7A20_CTRL1_XYZ_ENABLE);
    rc = sc7a20WriteRegister(device, SC7A20_REG_CTRL_REG1, ctrlReg1);
    if (SC7A20_OK != rc) {
        return rc;
    }

    /* ③ CTRL_REG4：BDU(bit7)=1 防高低字节撕裂 + FS[5:4] 量程 + HR(bit3)=1。
     *    HR 必须置位，否则器件停留在 10bit 模式，与下方右移 4 位、
     *    按 12bit 灵敏度换算的口径不一致。*/
    ctrlReg4 = (uint8_t)(SC7A20_CTRL4_BDU | SC7A20_CTRL4_HR
                         | (uint8_t)(sc7a20RangeBits(device->rangeG) << 4));
    rc = sc7a20WriteRegister(device, SC7A20_REG_CTRL_REG4, ctrlReg4);
    if (SC7A20_OK != rc) {
        return rc;
    }

    device->initialized = 1u;
    return SC7A20_OK;
}

int Sc7a20ReadRaw(Sc7a20* device, int16_t* x, int16_t* y, int16_t* z)
{
    int rc = SC7A20_OK;
    uint8_t buffer[SC7A20_OUTPUT_BYTES];

    rc = sc7a20CheckHandle(device);
    if (SC7A20_OK != rc) {
        return rc;
    }
    if ((NULL == x) || (NULL == y) || (NULL == z)) {
        return SC7A20_ERR_PARAM;
    }
    if (0u == device->initialized) {
        return SC7A20_ERR_NOT_READY;
    }

    *x = 0;
    *y = 0;
    *z = 0;

    /* 从 OUT_X_L 起、子地址置自增位，连续读 6 字节：
     * [0][1] = X 低/高，[2][3] = Y 低/高，[4][5] = Z 低/高 */
    rc = sc7a20ReadRegisterGroup(device,
                                 (uint8_t)(SC7A20_REG_OUT_X_L | SC7A20_AUTO_INCREMENT),
                                 buffer, SC7A20_OUTPUT_BYTES);
    if (SC7A20_OK != rc) {
        return rc;
    }

    *x = sc7a20ToSigned12(&buffer[0]);
    *y = sc7a20ToSigned12(&buffer[2]);
    *z = sc7a20ToSigned12(&buffer[4]);

    return SC7A20_OK;
}

int Sc7a20ReadMilliG(Sc7a20* device, int32_t* x, int32_t* y, int32_t* z)
{
    int rc = SC7A20_OK;
    int16_t rawX = 0;
    int16_t rawY = 0;
    int16_t rawZ = 0;
    int32_t sensitivity = 0;

    rc = sc7a20CheckHandle(device);
    if (SC7A20_OK != rc) {
        return rc;
    }
    if ((NULL == x) || (NULL == y) || (NULL == z)) {
        return SC7A20_ERR_PARAM;
    }

    *x = 0;
    *y = 0;
    *z = 0;

    /* 未初始化检查与取原始值都由 Sc7a20ReadRaw 完成，此处不重复访问总线 */
    rc = Sc7a20ReadRaw(device, &rawX, &rawY, &rawZ);
    if (SC7A20_OK != rc) {
        return rc;
    }

    /* 量程在构造期已校验，此处仅作防御 */
    sensitivity = sc7a20SensitivityMgPerLsb(device->rangeG);
    if (sensitivity <= 0) {
        return SC7A20_ERR_PARAM;
    }

    *x = (int32_t)rawX * sensitivity;
    *y = (int32_t)rawY * sensitivity;
    *z = (int32_t)rawZ * sensitivity;

    return SC7A20_OK;
}
