#ifndef _ACCEL_SERVICE_H
#define _ACCEL_SERVICE_H

/* ============================================================================
 * accel_service —— 加速度计采样服务（单结构体快照 + 独立采样线程）
 * ----------------------------------------------------------------------------
 * 职责（本模块只做三件事）：
 *   1) 持有注入的抽象总线 I2c 与内部创建的 Sc7a20 驱动实例，按固定周期采样；
 *   2) 用【唯一数据载体】AccelSample 保存最近一帧快照，采样线程是该快照的
 *      唯一写入方；
 *   3) 对外只提供「整帧拷贝」的读取入口 AccelServiceGet，绝不返回内部指针。
 *
 * 设计要点：
 *   - 依赖反转：本模块只认抽象头 i2c.h（注入）、sc7a20.h（驱动）、
 *     platform.h（线程 / 互斥 / 睡眠 / 计时抽象），不出现任何平台或 SDK 头，
 *     因此同一份代码在 ESP-IDF 与桌面仿真下都能编译；
 *   - 数据只用一个结构体：见 app_types.h 的 AccelSample（xMg / yMg / zMg /
 *     timestampMs / valid），不额外定义任何中间数据类型；
 *   - 生命周期：NewAccelService 完成全部装配（任一步失败完整回滚并返回 NULL），
 *     AccelServiceStart 拉起采样线程（幂等），DeleteAccelService 先让线程退出、
 *     再逆序销毁内部对象；注入进来的 I2c 总线所有权始终归装配方，本模块不销毁；
 *   - 线程与时钟一律走 platform 抽象：采样周期睡眠用 PlatformSleepInterface，
 *     时间戳用 PlatformTimestampInterface，线程用 ThreadObject；
 *   - 快照并发：采样线程在互斥量保护下整体写入，AccelServiceGet 在同一把互斥量
 *     保护下整体拷贝，调用方拿到的永远是「某一帧的完整副本」。
 *
 * 返回值约定（本模块所有 int 返回值统一）：
 *    0 : 成功
 *   -1 : 参数非法（service / outSample / config / config->bus 为空等）
 *   -2 : 未启动 / 尚无有效样本 / 线程未就绪
 * ========================================================================== */

#include <stdint.h>

#include "app_types.h"   /* AccelSample（唯一数据载体）、APP_ACCEL_POLL_PERIOD_MS */
#include "i2c.h"         /* I2c 抽象总线：由装配方注入，本模块只借用不销毁 */

#ifdef __cplusplus
extern "C" {
#endif

/* 抽象服务类型：具体结构体定义在 accel_service.c 内（对外不透明）。
 * 好处是公共头无需暴露 Sc7a20 / ThreadObject / PlatformMutex 等实现细节，
 * 上层只依赖本头的 4 个函数。 */
typedef struct AccelService AccelService;

/* ========== 装配参数（全部由装配区填写，构造阶段只读不存指针）==========
 * bus      ：【依赖注入】抽象 I2C 对象；所有权归装配方，本模块不销毁；
 *            必填，为空则构造直接失败（返回 NULL）。
 * address7 ：器件 7bit 地址，板级取 board_config.h 的
 *            BOARD_ACCEL_I2C_ADDRESS（当前为 0x19）；传 0 表示用模块默认值。
 * periodMs ：采样周期（毫秒）；传 0 表示用 APP_ACCEL_POLL_PERIOD_MS
 *            （当前 20 ms，对应 50 Hz 抽样，SC7A20 ODR 为 100 Hz）。
 */
typedef struct AccelServiceConfig {
    I2c*     bus;        /* 注入：抽象 I2C（装配区创建） */
    uint16_t address7;   /* 0x19 */
    uint32_t periodMs;   /* 采样周期，传 0 用 APP_ACCEL_POLL_PERIOD_MS */
} AccelServiceConfig;

/* ========== 模块级内存分配钩子（默认 malloc / free，可整体替换）==========
 * 替换须在创建任何 AccelService 之前执行，示例：
 *     accelServiceMalloc = pvPortMalloc;   accelServiceFree = vPortFree;
 * 说明：本模块一切堆分配 / 释放都走这两个钩子，禁止裸调 malloc / free。
 * 注：此处 size_t 由已包含的 i2c.h（其内部包含 stddef.h，用于声明同名钩子
 *     i2cMalloc）提供，因此本头不需要额外包含标准库头。 */
extern void* (*accelServiceMalloc)(size_t size);
extern void  (*accelServiceFree)(void* p);

/* ========== 构造 / 析构 ==========
 * NewAccelService  ：校验参数 -> 分配服务 -> 建互斥量 -> 取睡眠 / 计时函数集
 *                    -> NewSc7a20 + Sc7a20Init -> 建线程对象。
 *                    Sc7a20Init 失败【不算致命】：快照 valid 保持 0，线程照常
 *                    运行（便于现场观察「器件没应答」而不是整个服务起不来）。
 *                    任一步失败都完整回滚并返回 NULL。
 * DeleteAccelService：幂等。置退出标志 -> 停采样线程 -> 销毁线程对象 ->
 *                    销毁传感器 -> 销毁互斥量 -> 释放自身，并把调用方指针置
 *                    NULL 防悬空。【不销毁注入的 I2c 总线】。
 *                    可接受 NULL 指针或指向 NULL 的指针（直接返回）。 */
AccelService* NewAccelService(const AccelServiceConfig* config);
void          DeleteAccelService(AccelService** pService);

/* ========== 运行期操作 ==========
 * AccelServiceStart：拉起采样线程；幂等（已启动再调直接返回 0）。
 *     返回 0 成功；-1 参数非法；-2 线程对象未就绪或线程启动失败。
 *
 * AccelServiceGet  ：在互斥量保护下把当前快照【整体拷贝】给 outSample，
 *                    不返回内部指针，返回后调用方可自由使用该副本。
 *     返回 0  成功（拷到的快照 valid == 1，即最近一次读取成功）；
 *          -1  参数非法（service 或 outSample 为空）；
 *          -2  未启动（线程未就绪 / 未调用过 Start）；
 *              已启动但尚无有效样本（valid == 0，例如器件无应答）。
 *              注意：返回 -2 时只要服务已启动，outSample 也已被填入最新快照，
 *              调用方可据 valid 字段区分「没采到」与「上一帧读取失败」
 *              （读取失败时上一帧坐标刻意保留，仅把 valid 置 0）。 */
int AccelServiceStart(AccelService* service);
int AccelServiceGet(AccelService* service, AccelSample* outSample);

#ifdef __cplusplus
}
#endif

#endif /* _ACCEL_SERVICE_H */
