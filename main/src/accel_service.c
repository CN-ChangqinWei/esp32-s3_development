/* ============================================================================
 * accel_service.c —— 加速度计采样服务实现（单结构体 + 独立采样线程）
 *
 * 文件组织（自顶向下）：
 *   1) 包含与模块常量
 *   2) 具体实例结构体 struct AccelService 与模块级内存钩子
 *   3) 内部静态工具（互斥量加锁 / 解锁）
 *   4) 采样线程体 accelWorker（唯一写入 sample 的地方）
 *   5) 对外接口：构造 / 析构 / 启动 / 读快照
 *
 * 并发模型（关键）：
 *   - 写者只有采样线程一个（外加构造期的 memset 初始化，此时线程尚未存在）；
 *   - 读者是任意调用线程，经 AccelServiceGet 在同一把互斥量保护下整帧拷贝；
 *   - running 是 volatile uint8_t：Start / Delete / 采样线程自身会改它，
 *     读它用于「是否继续循环」的判断，属单字节标志位，无需额外加锁。
 *
 * 加锁点（全模块仅两处，且都在数据临界区，绝不跨睡眠 / I2C 事务）：
 *   A. 采样线程写入 sample（成功写入新值 / 失败仅清 valid）；
 *   B. AccelServiceGet 整帧拷出。
 *   两处都是「取锁 -> 几次赋值 -> 解锁」，临界区只有几十纳秒，不会阻塞或
 *   造成优先级反转式的长时间占用。
 *
 * 依赖（全部为抽象头，无任何平台 / SDK 头）：
 *   accel_service.h 本模块契约   app_types.h 数据载体
 *   sc7a20.h        加速度计驱动  i2c.h       抽象总线（注入）
 *   platform.h      线程 / 睡眠 / 计时 / 互斥抽象  thread_object.h 线程对象抽象
 * ========================================================================== */

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "accel_service.h"
#include "app_types.h"
#include "sc7a20.h"
#include "i2c.h"
#include "platform.h"
#include "thread_object.h"

/* 本模块强依赖 platform 的四项能力：线程对象 / 互斥量 / 睡眠 / 计时。
 * 任一被编译期裁剪都会让本文件无法实现采样线程，故此处显式拦截，
 * 避免出现「一堆未声明标识符」的间接报错。 */
#if !PLATFORM_USE_THREAD_OBJECT || !PLATFORM_USE_SYNC || \
    !PLATFORM_USE_SLEEP || !PLATFORM_USE_TIMESTAMP
#error "accel_service: 需要 platform 的 thread_object / sync / sleep / timestamp 能力（PLATFORM_USE_* 均须为 1）"
#endif

/* ---------------------------------------------------------------------------
 * 模块常量
 * ------------------------------------------------------------------------- */
/* 采样线程参数：栈 4096（FreeRTOS 侧按字映射，其余平台按字节）。
 * 线程体只做「一次 I2C 读 + 一次短临界区 + 一次睡眠」，栈深需求很小。 */
#define ACCEL_SERVICE_THREAD_STACK_SIZE   (4096)
#define ACCEL_SERVICE_THREAD_PRIORITY     (5)
#define ACCEL_SERVICE_THREAD_NAME         "accel_task"

/* 关闭流程：先给线程一个自然退出的窗口（轮询粒度 + 上限），
 * 超时仍未退出才调用线程对象的 stop「强制回收」，避免误杀正在做 I2C
 * 事务的线程。 */
#define ACCEL_SERVICE_STOP_POLL_MS        (5u)
#define ACCEL_SERVICE_STOP_TIMEOUT_MS     (500u)

/* 器件默认装配参数（address7 传 0 时使用；量程 / 速率与 SC7A20 驱动约定一致） */
#define ACCEL_SERVICE_DEFAULT_ADDRESS7    (0x19u)
#define ACCEL_SERVICE_RANGE_G             (2u)     /* 正负 2g，灵敏度 1 mg/LSB */
#define ACCEL_SERVICE_ODR_HZ              (100u)   /* 器件 ODR 100 Hz */

/* ---------------------------------------------------------------------------
 * 具体实例结构体（对外不透明，定义只在实现文件内）
 * ------------------------------------------------------------------------- */
struct AccelService {
    I2c*                       bus;      /* 注入的抽象总线：仅借用，析构时不销毁 */
    Sc7a20*                    sensor;   /* 驱动实例：本模块创建、本模块销毁 */
    ThreadObject*              thread;   /* 采样线程（抽象线程对象） */
    PlatformMutex*             mutex;    /* 保护 sample 快照的互斥量 */
    PlatformSleepInterface     sleep;    /* 平台睡眠函数集（无堆分配） */
    PlatformTimestampInterface clock;    /* 平台毫秒计时函数集（无堆分配） */
    AccelSample                sample;   /* 【唯一数据载体】最近一帧快照 */
    uint32_t                   periodMs; /* 采样周期（毫秒），构造期已归一化为 > 0 */
    volatile uint8_t           running;  /* 1 = 采样线程应继续运行；0 = 请求退出 */
};

/* ---------------------------------------------------------------------------
 * 模块级内存钩子（默认 malloc / free，可被外部整体替换）
 * ------------------------------------------------------------------------- */
void* (*accelServiceMalloc)(size_t size) = malloc;
void  (*accelServiceFree)(void* p)       = free;

/* ---------------------------------------------------------------------------
 * 内部工具：互斥量加锁 / 解锁（含空值与空槽位防御）
 * ------------------------------------------------------------------------- */
static int accelServiceLock(AccelService* self)
{
    if (NULL == self || NULL == self->mutex) {
        return -1;
    }
    if (NULL == self->mutex->interfaces.lock) {
        return -1;
    }
    return (0 == self->mutex->interfaces.lock(self->mutex->instance)) ? 0 : -1;
}

static void accelServiceUnlock(AccelService* self)
{
    if (NULL == self || NULL == self->mutex) {
        return;
    }
    if (NULL == self->mutex->interfaces.unlock) {
        return;
    }
    (void)self->mutex->interfaces.unlock(self->mutex->instance);
}

/* ---------------------------------------------------------------------------
 * 采样线程体：本模块对 sample 的唯一写入方
 * ------------------------------------------------------------------------- */
static void accelWorker(void* param)
{
    AccelService* self = (AccelService*)param;
    uint32_t      periodMs;

    if (NULL == self) {
        log_error("accel_service worker: param is null");
        return;
    }

    /* 周期在构造期已归一化，这里再兜一层，绝不让 sleepMs(0) 变成忙等 */
    periodMs = (self->periodMs > 0u) ? self->periodMs : (uint32_t)APP_ACCEL_POLL_PERIOD_MS;

    while (self->running) {
        int32_t xMg  = 0;
        int32_t yMg  = 0;
        int32_t zMg  = 0;
        int     ret  = Sc7a20ReadMilliG(self->sensor, &xMg, &yMg, &zMg);

        /* 加锁点 A：只有「写 sample」这一小段在临界区内，
         * I2C 事务与周期睡眠都在锁外，避免读者被总线耗时拖住。 */
        if (0 == accelServiceLock(self)) {
            if (0 == ret) {
                self->sample.xMg         = xMg;
                self->sample.yMg         = yMg;
                self->sample.zMg         = zMg;
                self->sample.timestampMs = (uint32_t)self->clock.milliseconds();
                self->sample.valid       = 1u;
            } else {
                /* 读取失败：上一帧坐标刻意保留，只把 valid 置 0，
                 * 让上层能区分「没采到」与「器件刚失联」 */
                self->sample.valid = 0u;
            }
            accelServiceUnlock(self);
        } else {
            log_error("accel_service worker: lock failed, drop this frame (ret=%d)", ret);
        }

        self->sleep.sleepMs(periodMs);
    }

    /* 循环退出后清标志（幂等）：让 DeleteAccelService 能观察到「线程已自然退出」 */
    self->running = 0u;
    log_info("accel_service worker: exit");
}

/* ---------------------------------------------------------------------------
 * 构造：全部装配动作在此完成，任一步失败完整回滚
 * ------------------------------------------------------------------------- */
AccelService* NewAccelService(const AccelServiceConfig* config)
{
    AccelService*        self      = NULL;
    Sc7a20Config         sensorCfg;
    PlatformThreadConfig threadCfg;
    int                  initRet   = 0;

    ASSERT_NOT_NULL(config, "accel_service: config is null", NULL);
    ASSERT_NOT_NULL(config->bus, "accel_service: config->bus is null", NULL);

    if (NULL == accelServiceMalloc) {
        log_error("NewAccelService: accelServiceMalloc is null");
        return NULL;
    }

    self = (AccelService*)accelServiceMalloc(sizeof(AccelService));
    if (NULL == self) {
        log_error("NewAccelService: alloc service failed");
        return NULL;
    }
    /* 清零即完成 sample 初始化：xMg / yMg / zMg / timestampMs = 0，valid = 0，
     * 语义为「尚未采到有效样本」 */
    memset(self, 0, sizeof(AccelService));

    self->bus      = config->bus;   /* 仅借用，所有权仍在装配方 */
    self->periodMs = (config->periodMs > 0u) ? config->periodMs
                                             : (uint32_t)APP_ACCEL_POLL_PERIOD_MS;
    self->running  = 0u;

    /* ---- 1) 互斥量：保护 sample 快照 ---- */
    self->mutex = NewPlatformMutexForPlatform();
    if (NULL == self->mutex) {
        log_error("NewAccelService: create mutex failed");
        goto rollback;
    }

    /* ---- 2) 平台睡眠 / 计时函数集（纯函数表，不涉及堆分配） ---- */
    self->sleep = NewPlatformSleep();
    self->clock = NewPlatformTimestamp();
    if (NULL == self->sleep.sleepMs || NULL == self->clock.milliseconds) {
        log_error("NewAccelService: platform sleep/timestamp unavailable");
        goto rollback;
    }

    /* ---- 3) 加速度计：构造（不访问总线） + Init（真正下发配置） ---- */
    memset(&sensorCfg, 0, sizeof(sensorCfg));
    sensorCfg.bus            = config->bus;
    sensorCfg.address7       = (config->address7 != 0u) ? config->address7
                                                        : (uint16_t)ACCEL_SERVICE_DEFAULT_ADDRESS7;
    sensorCfg.expectedWhoAmI = 0u;   /* 不校验身份：SC7A20HTR 无公开数据手册 */
    sensorCfg.rangeG         = (uint8_t)ACCEL_SERVICE_RANGE_G;
    sensorCfg.odrHz          = (uint16_t)ACCEL_SERVICE_ODR_HZ;

    self->sensor = NewSc7a20(&sensorCfg);
    if (NULL == self->sensor) {
        log_error("NewAccelService: create sc7a20 failed (addr7=0x%02X)",
                  (unsigned)sensorCfg.address7);
        goto rollback;
    }

    initRet = Sc7a20Init(self->sensor);
    if (0 != initRet) {
        /* 不致命：器件未应答 / 上电时序未稳时，服务照常构建，快照保持
         * valid == 0，采样线程仍按周期尝试读取，便于现场观察故障而非
         * 让整个应用起不来 */
        log_warn("NewAccelService: sc7a20 init failed (ret=%d), snapshot stays invalid",
                 initRet);
    }

    /* ---- 4) 采样线程对象：workFunc 留空，线程体与实参在 start 时注入 ---- */
    memset(&threadCfg, 0, sizeof(threadCfg));
    threadCfg.stackSize = ACCEL_SERVICE_THREAD_STACK_SIZE;
    threadCfg.priority  = ACCEL_SERVICE_THREAD_PRIORITY;
    threadCfg.name      = ACCEL_SERVICE_THREAD_NAME;
    threadCfg.workFunc  = NULL;
    threadCfg.workParam = NULL;

    self->thread = NewPlatformThreadObject(&threadCfg);
    if (NULL == self->thread) {
        log_error("NewAccelService: create thread object failed");
        goto rollback;
    }

    log_info("NewAccelService: ready (addr7=0x%02X, period=%lu ms, init=%d)",
             (unsigned)self->sensor->address7, (unsigned long)self->periodMs, initRet);
    return self;

rollback:
    /* 完整回滚：DeleteAccelService 对半成品同样安全（各字段为空则跳过） */
    DeleteAccelService(&self);
    return NULL;
}

/* ---------------------------------------------------------------------------
 * 析构：置退出 -> 停线程 -> 逆序销毁内部对象 -> 释放自身
 * ------------------------------------------------------------------------- */
void DeleteAccelService(AccelService** pService)
{
    AccelService* self = NULL;
    int           waitedMs;

    if (NULL == pService) {
        return;
    }
    self = *pService;
    if (NULL == self) {
        return;
    }

    /* ---- 1) 请求采样线程退出（不再有新的 sample 写入） ---- */
    self->running = 0u;

    /* ---- 2) 回收线程：先等自然退出，超时才强制 stop ---- */
    if (NULL != self->thread) {
        if (NULL != self->thread->interfaces.isRun) {
            waitedMs = 0;
            while (0 != self->thread->interfaces.isRun(self->thread->instance) &&
                   waitedMs < (int)ACCEL_SERVICE_STOP_TIMEOUT_MS) {
                if (NULL == self->sleep.sleepMs) {
                    break;   /* 半成品：没有睡眠能力就不等了 */
                }
                self->sleep.sleepMs(ACCEL_SERVICE_STOP_POLL_MS);
                waitedMs += (int)ACCEL_SERVICE_STOP_POLL_MS;
            }
        }

        if (NULL != self->thread->interfaces.isRun &&
            0 != self->thread->interfaces.isRun(self->thread->instance)) {
            /* 兜底：线程卡在某次 I2C 事务里，无法在时限内自然退出。
             * 强制回收后仍会走到 DeleteThreadObject，不会残留任务。 */
            log_warn("DeleteAccelService: worker not exit in %u ms, force stop",
                     (unsigned)ACCEL_SERVICE_STOP_TIMEOUT_MS);
            if (NULL != self->thread->interfaces.stop) {
                (void)self->thread->interfaces.stop(self->thread->instance);
            }
        }

        /* 释放线程对象（内部会先销毁具象实例再释放抽象对象） */
        DeleteThreadObject(self->thread);
        self->thread = NULL;
    }

    /* ---- 3) 传感器实例（只释放自己，不动注入的总线） ---- */
    DeleteSc7a20(&self->sensor);

    /* ---- 4) 互斥量：此时已无生产者与消费者，可安全销毁 ---- */
    DeletePlatformMutex(&self->mutex);

    /* ---- 5) 解除对总线的借用（【禁止】DeleteI2c：总线归装配方所有） ---- */
    self->bus = NULL;

    /* ---- 6) 释放自身并置空调用方指针，防悬空 ---- */
    if (NULL != accelServiceFree) {
        accelServiceFree(self);
    }
    self = NULL;
    *pService = NULL;
}

/* ---------------------------------------------------------------------------
 * 启动采样线程（幂等）
 * ------------------------------------------------------------------------- */
int AccelServiceStart(AccelService* service)
{
    AccelService* self = service;
    int           err;

    ASSERT_NOT_NULL(service, "accel_service start: service is null", -1);

    if (NULL == self->thread || NULL == self->thread->interfaces.start) {
        log_error("accel_service start: thread object not ready");
        return -2;
    }

    if (0u != self->running) {
        return 0;   /* 幂等：已启动 */
    }

    /* 先置运行标志再拉起线程：线程一进入循环就能观察到 running == 1，
     * 不会出现「刚创建就立即空转退出」的竞态 */
    self->running = 1u;

    err = self->thread->interfaces.start(self->thread->instance, accelWorker, self);
    if (0 != err) {
        self->running = 0u;
        log_error("accel_service start: thread start failed (ret=%d)", err);
        return -2;
    }

    log_info("accel_service start: accel_task running (period=%lu ms)",
             (unsigned long)self->periodMs);
    return 0;
}

/* ---------------------------------------------------------------------------
 * 读快照：互斥量保护下整帧拷贝，绝不返回内部指针
 * ------------------------------------------------------------------------- */
int AccelServiceGet(AccelService* service, AccelSample* outSample)
{
    AccelService* self = service;

    ASSERT_NOT_NULL(service, "accel_service get: service is null", -1);
    ASSERT_NOT_NULL(outSample, "accel_service get: outSample is null", -1);

    if (NULL == self->thread || 0u == self->running) {
        return -2;   /* 未启动 */
    }

    /* 加锁点 B：整帧拷贝在临界区内完成，调用方拿到的是完整一致的副本 */
    if (0 != accelServiceLock(self)) {
        log_error("accel_service get: lock failed");
        return -2;
    }

    memcpy(outSample, &self->sample, sizeof(AccelSample));

    accelServiceUnlock(self);

    /* 已启动但尚无有效样本（器件无应答 / 尚未采到第一帧）：
     * 副本仍交给调用方（内含上一帧坐标），用 -2 与 valid == 0 双通道告知 */
    return (0u != outSample->valid) ? 0 : -2;
}
