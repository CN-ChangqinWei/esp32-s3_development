#ifndef _SC7A20_H
#define _SC7A20_H

/* ============================================================================
 * Sc7a20 —— 三轴数字加速度计驱动（板上 U8 = SC7A20HTR）
 * ----------------------------------------------------------------------------
 * 设计要点：
 *   1. 平台无关：本模块只依赖【抽象 I2C】i2c.h，不引用 driver/xxx.h、
 *      freertos/xxx.h、esp_xxx.h，因此装配方注入 I2c 抽象对象即可运行。
 *   2. 依赖注入：总线句柄与器件 7bit 地址全部走 Sc7a20Config 传入，
 *      构造阶段【不访问总线】，便于“上电前装配、上电后 Init”。
 *   3. 生命周期：NewSc7a20 只做参数校验与字段落地；
 *      Sc7a20Init 才真正读 WHO_AM_I 并下发 CTRL_REG1 / CTRL_REG4。
 *      DeleteSc7a20 不销毁 I2c（总线所有权归装配方）。
 *
 * 寄存器映射（寄存器布局与 LIS2DH / LIS2DH12 系列兼容，待实测确认）：
 *   WHO_AM_I   0x0F  只读，器件标识
 *   CTRL_REG1  0x20  ODR[7:4] + LPen(bit3) + XYZ enable[2:0]，使能位 0b111
 *   CTRL_REG4  0x23  BDU(bit7) + FS[5:4] + HR(bit3)
 *                    FS：00=正负2g、01=正负4g、10=正负8g、11=正负16g
 *   OUT_X_L    0x28  输出起始地址；多字节读须把子地址最高位置 1 触发自增，
 *                    自增后连续 6 字节 = X 低、X 高、Y 低、Y 高、Z 低、Z 高
 *                    （每轴 2 字节小端），硬件为 12bit 左对齐。
 *
 * 数值换算：
 *   原始值 raw12 = 16 位小端值算术右移 4 位（去掉左对齐的低 4 个填充位）。
 *   灵敏度（针对 12bit 值）：正负2g 为 1 mg/LSB、正负4g 为 2、正负8g 为 4、
 *   正负16g 为 12。
 *
 * 返回值约定（本模块所有 int 返回值）：
 *    0 : 成功
 *   -1 : 参数非法（device 为空、bus 为空、输出指针为空、量程或速率非法等）
 *   -2 : 未初始化 / 该能力未实现（未成功调用过 Sc7a20Init）
 *   -3 : 器件无应答 / 身份不符（I2C 抽象层返回的任何负值统一映射到此处；
 *        也包含 WHO_AM_I 与 expectedWhoAmI 不一致的情况）
 * ========================================================================== */

#include <stdint.h>
#include "i2c.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========== 内存分配钩子（默认 malloc / free，可被外部整体替换）==========
 * 替换示例（须在创建任何对象之前执行）：
 *     sc7a20Malloc = pvPortMalloc;   sc7a20Free = vPortFree;
 */
extern void* (*sc7a20Malloc)(size_t size);
extern void  (*sc7a20Free)(void* p);

/* ========== 装配参数 ==========
 * bus           ：【依赖注入】抽象 I2C 对象，所有权归装配方，本模块不销毁。
 * address7      ：器件 7bit 地址，板级为 0x19（见 BOARD_ACCEL_I2C_ADDRESS）；
 *                 I2C 抽象层内部自行左移，本模块按 7bit 原值传入。
 * expectedWhoAmI：期望的 WHO_AM_I；传 0 表示不校验（首版装配建议传 0，
 *                 先用 Sc7a20ReadWhoAmI 实测打印，再固化为常量）。
 *                 待实测确认：SC7A20HTR 无公开数据手册，无法给出确定的标识值；
 *                 若按 LIS2DH12 兼容推测为 0x33，与实物不符时以实测值为准。
 * rangeG        ：量程，仅接受 2 / 4 / 8 / 16。
 * odrHz         ：输出数据率，仅接受 1 / 10 / 25 / 50 / 100 / 200 / 400。
 */
typedef struct Sc7a20Config {
    I2c*     bus;             /* 依赖注入：抽象 I2C */
    uint16_t address7;        /* 7bit 地址，板级为 0x19 */
    uint8_t  expectedWhoAmI;  /* 期望的 WHO_AM_I；传 0 表示不校验 */
    uint8_t  rangeG;          /* 2 / 4 / 8 / 16 */
    uint16_t odrHz;           /* 1 / 10 / 25 / 50 / 100 / 200 / 400。
                               * 必须用 16 位：400 超出 uint8_t 上限(255)，
                               * 用 8 位会被截断成 144，导致 400Hz 档永久失效 */
} Sc7a20Config;

typedef struct Sc7a20 Sc7a20;

/* ========== 具体实例结构体（字段中性：不含任何引脚号 / 端口号）========== */
struct Sc7a20 {
    I2c*     bus;            /* 抽象总线，仅借用，不负责释放 */
    uint16_t address7;       /* 器件 7bit 地址 */
    uint8_t  expectedWhoAmI; /* 0 表示不校验身份 */
    uint8_t  rangeG;         /* 已校验的量程 2 / 4 / 8 / 16 */
    uint16_t odrHz;          /* 已校验的速率 1 / 10 / 25 / 50 / 100 / 200 / 400 */
    uint8_t  initialized;    /* 1 = Sc7a20Init 已成功；0 = 未初始化 */
};

/* ========== 构造 / 析构 ==========
 * NewSc7a20：分配实例 + 校验配置 + 落地字段；失败返回 NULL 且已回滚。
 *            构造阶段不访问总线（不发任何 I2C 事务）。
 * DeleteSc7a20：释放实例并把调用者指针置空；【不销毁 bus】。
 */
Sc7a20* NewSc7a20(const Sc7a20Config* config);
void    DeleteSc7a20(Sc7a20** pDevice);

/* ========== 运行期操作 ========== */
/* 校验 WHO_AM_I（expectedWhoAmI 非 0 时）并写入 CTRL_REG1 / CTRL_REG4；
 * 成功置 initialized = 1；可重复调用（幂等，且可恢复器件意外复位后的配置）。*/
int Sc7a20Init(Sc7a20* device);

/* 只读 WHO_AM_I，不需要先 Init（Init 内部用它做身份校验）。*/
int Sc7a20ReadWhoAmI(Sc7a20* device, uint8_t* whoAmI);

/* 读三轴原始值：连续 6 字节小端，算术右移 4 位得到 12bit 有符号值。*/
int Sc7a20ReadRaw(Sc7a20* device, int16_t* x, int16_t* y, int16_t* z);

/* 读三轴加速度，按当前量程换算为毫 g（mg）。*/
int Sc7a20ReadMilliG(Sc7a20* device, int32_t* x, int32_t* y, int32_t* z);

#ifdef __cplusplus
}
#endif

#endif /* _SC7A20_H */
