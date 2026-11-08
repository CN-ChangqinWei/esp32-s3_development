/* =====================================================================
 * event_loop —— 「任务事件循环」积木（队列 + 指定线程执行）
 *
 * 定位：libs/ 下的通用积木，与 thread_object / timer_executor 平级；
 *       后续 imgui view / model / demo 均按 event_loop.h 的【冻结契约】对接。
 *
 * 结构（OOP in C）：
 *   - 抽象层（event_loop.h，冻结契约）：EventLoop{ void* instance;
 *     EventLoopInterface interfaces; }，配抽象构造 NewEventLoop /
 *     就地注入 InitEventLoop / 析构 DeleteEventLoop。抽象层【不提供
 *     任何默认实现】：虚表 6 槽（post / start / stop / runOnce /
 *     isRun / deleteInstance）全部由具象提供。
 *   - 具象层（本文件内的 EventLoopImpl，【绝不进头文件】，口径同
 *     PlatformWebSocketWindows）：环形任务队列 + PlatformMutex +
 *     【借用的】ThreadObject。
 *
 * 并发模型（照 libs/platform/src/platform_websocket.c 的 wsQueue* 系列，
 * 但那是 static 私有，本模块做成公开积木）：
 *   - 生产者：任意线程调 post() 入队；
 *   - 消费者：event_loop 所属线程的 worker() 出队执行；
 *   - 需要同步的【只有队列本体】（head / tail / count + 槽位），由本模块
 *     自持的 PlatformMutex 保护；isQuit / isRun 是跨线程标志（字段保持
 *     volatile int，读写一律经 EVENT_LOOP_ATOMIC_STORE / EVENT_LOOP_ATOMIC_LOAD
 *     宏，禁止裸读写）；
 *   - 任务执行【绝不能在持锁状态下】进行 —— 否则 post() 会被一个长任务
 *     长时间挡住（worker 每次都是「持锁取一个 → 解锁 → 锁外执行」）。
 *
 * 停止语义（已取证：ThreadObject::stop() 是硬杀 TerminateThread /
 * pthread_cancel，没有协作退出；本仓库也没有信号量 / 条件变量 / 事件
 * 原语 —— 跨线程标志只能走 __atomic_*_n 内建，见 event_loop.h 的
 * EVENT_LOOP_ATOMIC_STORE / EVENT_LOOP_ATOMIC_LOAD 宏）：
 *   - 本模块自己做「置 isQuit → 有界轮询 isRun → 超时才强停」：
 *       · stop()        ：置 isQuit + 有界等待（每拍 sleepMs(2)，
 *                         上限 EVENT_LOOP_STOP_TIMEOUT_MS）；超时只
 *                         log_warn 并返回 <0，【不】在此强杀；
 *       · deleteInstance：先走一次同样的有界等待；仍超时才经【借用的】
 *                         ThreadObject 强停（冻结契约承诺的「兜底强停」，
 *                         也是释放实例本体前的唯一安全手段 —— 否则就是
 *                         对仍在运行的 worker 做 use-after-free），然后
 *                         才销毁互斥量、释放实例。
 *   - 队列空时 worker 只能 sleepMs 让出 CPU（用户已确认此方案；不去
 *     platform 层造新原语）。
 *
 * 内存：一切堆分配走本模块分配器 eventLoopMalloc / eventLoopFree
 *       （默认经 NewPlatformMemory() 派发，可被外部整体替换）；
 *       【绝不】裸调 malloc/free。
 *
 * 命名：本文件为纯 C，实例局部变量一律命名 self（禁用 this）；具象函数
 *       一律带 eventLoop 前缀（规避 Windows SDK 短名污染）。
 * ===================================================================== */

#include "event_loop.h"
#include <string.h>     /* memset / memcpy / strlen */

/* ---------------------------------------------------------------------
 * 本模块内部常量（不进冻结头文件）
 * ------------------------------------------------------------------- */
/* stop() / deleteInstance 有界轮询的「拍长」（毫秒）。
 * 冻结头里的 EVENT_LOOP_STOP_TIMEOUT_MS 是【总上限】，本值是单拍睡眠长度：
 * 取 2 与 EVENT_LOOP_IDLE_SLEEP_MS 同量级 —— 足够小（到点即察觉退出），
 * 又足够大（2000 ms 上限内最多约 1000 拍，CPU 开销可忽略）。 */
#define EVENT_LOOP_STOP_POLL_MS 2

/* ---------------------------------------------------------------------
 * 模块内存分配器（依赖反转）
 *   默认落到「当前平台」的内存管理能力上；NewPlatformMemory() 只返回
 *   一组静态函数指针（无堆分配、无状态），开销可忽略。仍可由外部在
 *   创建对象前整体替换 eventLoopMalloc / eventLoopFree。
 * ------------------------------------------------------------------- */
static void* eventLoopPlatformMalloc(size_t size)
{
    return NewPlatformMemory().malloc(size);
}

static void eventLoopPlatformFree(void* p)
{
    NewPlatformMemory().free(p);
}

void* (*eventLoopMalloc)(size_t size) = eventLoopPlatformMalloc;
void  (*eventLoopFree)(void* p)       = eventLoopPlatformFree;

/* =====================================================================
 * 抽象层（与平台无关；只做「分配 + 装配抽象对象」，不填充虚表槽位）
 * ===================================================================== */

EventLoop* NewEventLoop(void* instance, EventLoopInterface interfaces)
{
    EventLoop* loop;

    ASSERT_NOT_NULL(instance, "NewEventLoop: instance is null", NULL);

    loop = eventLoopMalloc(sizeof(EventLoop));
    ASSERT_NOT_NULL(loop, "NewEventLoop: eventLoopMalloc failed", NULL);

    /* 交由 Init 完成装配（只设置 instance/interfaces，禁止手工赋值） */
    if (InitEventLoop(loop, instance, interfaces) != 0) {
        eventLoopFree(loop);
        loop = NULL;
    }

    return loop;
}

int InitEventLoop(EventLoop* loop, void* instance, EventLoopInterface interfaces)
{
    ASSERT_NOT_NULL(loop, "InitEventLoop: loop is null", -1);
    ASSERT_NOT_NULL(instance, "InitEventLoop: instance is null", -2);

    loop->instance    = instance;
    loop->interfaces  = interfaces;

    return 0;
}

void DeleteEventLoop(EventLoop** pLoop)
{
    EventLoop* loop;

    if (pLoop == NULL) {
        return;
    }

    loop = *pLoop;
    if (loop == NULL) {
        return;
    }

    /* 先销毁具体实例（传具体实例，不是抽象包装对象） */
    if (loop->interfaces.deleteInstance != NULL) {
        loop->interfaces.deleteInstance(loop->instance);
        loop->instance = NULL;
    }

    /* 再释放抽象对象，最后置空调用方指针，防悬空 */
    eventLoopFree(loop);
    *pLoop = NULL;
}

/* =====================================================================
 * 具象实现实例（只在本编译单元内可见，绝不进头文件）
 * ===================================================================== */
typedef struct EventLoopImpl{
    ThreadObject*  thread;   /* 借用的线程对象：所有权【不转移】，本模块只借用、不负责销毁 */
    PlatformMutex* mutex;    /* 保护任务队列的互斥量：本模块持有所有权，析构时销毁 */

    EventLoopTask  tasks[EVENT_LOOP_TASK_CAP];    /* 环形队列：任务槽 */
    void*          params[EVENT_LOOP_TASK_CAP];   /* 环形队列：任务实参槽 */
    int            head;      /* 出队下标 0..CAP-1；仅持锁访问 */
    int            tail;      /* 入队下标 0..CAP-1；仅持锁访问 */
    int            count;     /* 当前任务数 0..CAP；仅持锁访问 */

    /* 跨线程标志（「外部线程」写 stop/start、读 isRun；「worker 线程」读写）：
     * 一律经 EVENT_LOOP_ATOMIC_STORE（release 写）/ EVENT_LOOP_ATOMIC_LOAD
     * （acquire 读）宏访问，【禁止】裸读写 —— volatile 不保证原子性与内存
     * 序，裸读写属数据竞争（UB）。字段保持 volatile int：__atomic_*_n 内建
     * 接受 volatile 限定指针，且非 GCC/Clang 退化路径不劣于改动前。 */
    volatile int   isQuit;    /* 1 = 请求 worker 退出（stop / deleteInstance 写入） */
    volatile int   isRun;     /* 1 = 循环正在跑（worker 自行置位 / 落零） */

    char           name[EVENT_LOOP_NAME_CAP];     /* 循环名（仅诊断用，可空串） */
} EventLoopImpl;

/* ---------------------------------------------------------------------
 * 具象函数前置声明（static，仅本编译单元可见）
 * ------------------------------------------------------------------- */
static int   eventLoopImplPost(void* instance, EventLoopTask task, void* param);
static int   eventLoopImplStart(void* instance, struct ThreadObject* threadObj);
static int   eventLoopImplStop(void* instance);
static int   eventLoopImplRunOnce(void* instance);
static int   eventLoopImplIsRun(void* instance);
static void  eventLoopImplDeleteInstance(void* instance);
static void  eventLoopWorker(void* param);

static void            eventLoopLock(EventLoopImpl* self);
static void            eventLoopUnlock(EventLoopImpl* self);
static void            eventLoopSleepMs(uint32_t ms);
static int             eventLoopWaitFlagZero(volatile int* flag, uint32_t timeoutMs);
static EventLoopInterface eventLoopInterface(void);

/* ---------------------------------------------------------------------
 * 小型工具：队列互斥量的持锁 / 解锁
 *   口径与 platform_websocket.c 的 wsQueueLock / wsQueueUnlock 一致：
 *   判空后调用，返回值忽略（platform_sync 的 lock 失败即视为平台故障，
 *   此时无恢复手段；调用点不因「加锁失败」改变语义）。
 * ------------------------------------------------------------------- */
static void eventLoopLock(EventLoopImpl* self)
{
    if (self == NULL || self->mutex == NULL) {
        return;
    }
    if (self->mutex->interfaces.lock != NULL) {
        (void)self->mutex->interfaces.lock(self->mutex->instance);
    }
}

static void eventLoopUnlock(EventLoopImpl* self)
{
    if (self == NULL || self->mutex == NULL) {
        return;
    }
    if (self->mutex->interfaces.unlock != NULL) {
        (void)self->mutex->interfaces.unlock(self->mutex->instance);
    }
}

/* 睡眠 ms（platform_sleep 能力若被裁剪，函数指针为空时安静返回） */
static void eventLoopSleepMs(uint32_t ms)
{
    PlatformSleepInterface sleep = NewPlatformSleep();

    if (sleep.sleepMs != NULL) {
        sleep.sleepMs(ms);
    }
}

/* ---------------------------------------------------------------------
 * 有界等待 *flag 落零（stop / deleteInstance 共用的「协作退出」原语）。
 *   返回 0 = 已落零；<0 = 超时（flag 可能仍为非零，循环可能仍在跑）。
 *   时间源优先取 platform_timestamp（单调 milliseconds）；万一该能力被
 *   裁剪（milliseconds == NULL），退回「拍数」有界轮询，绝不退化成无界
 *   忙等（每拍仍 sleepMs，拍数 = timeoutMs / 每拍毫秒数，至少 1 拍）。
 *   flag 的读取一律经 EVENT_LOOP_ATOMIC_LOAD（acquire），与写方的
 *   EVENT_LOOP_ATOMIC_STORE（release）配对。
 * ------------------------------------------------------------------- */
static int eventLoopWaitFlagZero(volatile int* flag, uint32_t timeoutMs)
{
    PlatformTimestampInterface ts;
    uint64_t                   t0;
    uint32_t                   rounds;

    if (flag == NULL) {
        return -1;
    }

    ts = NewPlatformTimestamp();

    if (ts.milliseconds != NULL) {
        t0 = ts.milliseconds();
        while (EVENT_LOOP_ATOMIC_LOAD(flag) != 0) {
            eventLoopSleepMs(EVENT_LOOP_STOP_POLL_MS);
            if ((ts.milliseconds() - t0) >= (uint64_t)timeoutMs) {
                return -2;                      /* 超时 */
            }
        }
        return 0;
    }

    /* 无时间源的兜底：按拍数有界轮询（至少 1 拍，保证「等过」） */
    rounds = timeoutMs / (uint32_t)EVENT_LOOP_STOP_POLL_MS;
    if (rounds == 0) {
        rounds = 1;
    }
    while (EVENT_LOOP_ATOMIC_LOAD(flag) != 0 && rounds > 0) {
        eventLoopSleepMs(EVENT_LOOP_STOP_POLL_MS);
        rounds--;
    }

    return (EVENT_LOOP_ATOMIC_LOAD(flag) == 0) ? 0 : -3;
}

/* =====================================================================
 * 具象实现：post（生产者入口，任意线程可调）
 * ===================================================================== */
static int eventLoopImplPost(void* instance, EventLoopTask task, void* param)
{
    EventLoopImpl* self;

    ASSERT_NOT_NULL(instance, "eventLoop post: instance is null", -1);
    ASSERT_NOT_NULL(task, "eventLoop post: task is null", -2);

    self = (EventLoopImpl*)instance;

    eventLoopLock(self);
    if (self->count >= EVENT_LOOP_TASK_CAP) {
        eventLoopUnlock(self);
        log_warn("eventLoop post: task queue is FULL (cap=%d) —— the new task is DROPPED "
                 "(loop name=\"%s\"); typical causes: the loop thread is stalled inside a long "
                 "task, or the producer outpaces the consumer",
                 EVENT_LOOP_TASK_CAP, self->name);
        return -3;
    }

    /* 先填槽、后推进下标与计数：消费者只在持锁状态下读槽，不会读到半成品 */
    self->tasks[self->tail]  = task;
    self->params[self->tail] = param;
    self->tail = (self->tail + 1) % EVENT_LOOP_TASK_CAP;
    self->count++;

    eventLoopUnlock(self);

    return 0;
}

/* =====================================================================
 * 具象实现：start（在外部注入的 ThreadObject 上启动循环）
 * ===================================================================== */
static int eventLoopImplStart(void* instance, struct ThreadObject* threadObj)
{
    EventLoopImpl* self;
    int            rc;

    ASSERT_NOT_NULL(instance, "eventLoop start: instance is null", -1);
    ASSERT_NOT_NULL(threadObj, "eventLoop start: threadObj is null (the caller must inject a "
                               "ThreadObject; its ownership is NOT transferred)", -2);

    self = (EventLoopImpl*)instance;

    if (EVENT_LOOP_ATOMIC_LOAD(&self->isRun) != 0) {
        log_error("eventLoop start: the loop is ALREADY running (loop name=\"%s\")", self->name);
        return -3;
    }
    if (threadObj->instance == NULL || threadObj->interfaces.start == NULL) {
        log_error("eventLoop start: the injected ThreadObject is not assembled "
                  "(instance=%s start=%s; loop name=\"%s\")",
                  (threadObj->instance != NULL) ? "ok" : "null",
                  (threadObj->interfaces.start != NULL) ? "ok" : "null",
                  self->name);
        return -4;
    }

    /* 复位退出请求：本分支已确认 isRun == 0（无存活 worker），故此处清零
     * isQuit 是安全的；它同时兜住「曾在未运行时调过 stop() 留下 isQuit=1」
     * 导致新 worker 一进循环就立即退出的陷阱。 */
    EVENT_LOOP_ATOMIC_STORE(&self->isQuit, 0);

    /* 只借用，不持有所有权：不负责销毁 threadObj */
    self->thread = threadObj;

    rc = threadObj->interfaces.start(threadObj->instance, eventLoopWorker, self);
    if (rc != 0) {
        self->thread = NULL;
        log_error("eventLoop start: ThreadObject::start failed rc=%d (loop name=\"%s\")",
                  rc, self->name);
        return -5;
    }

    return 0;
}

/* =====================================================================
 * 具象实现：stop（请求退出 + 有界等待；超时不强杀，交给 deleteInstance 兜底）
 * ===================================================================== */
static int eventLoopImplStop(void* instance)
{
    EventLoopImpl* self;

    ASSERT_NOT_NULL(instance, "eventLoop stop: instance is null", -1);

    self = (EventLoopImpl*)instance;

    /* 未在运行（含从未启动）：isQuit 置位后 isRun 已是 0，有界等待立即
     * 完成，返回 0（幂等）。残留的 isQuit=1 由下一次 start() 复位。 */
    EVENT_LOOP_ATOMIC_STORE(&self->isQuit, 1);

    if (eventLoopWaitFlagZero(&self->isRun, EVENT_LOOP_STOP_TIMEOUT_MS) != 0) {
        log_warn("eventLoop stop: timed out waiting for the loop to exit "
                 "(timeout=%d ms, loop name=\"%s\") —— the loop may STILL be running; "
                 "it is NOT force-killed here (deleteInstance will do that as the last resort)",
                 EVENT_LOOP_STOP_TIMEOUT_MS, self->name);
        return -2;
    }

    return 0;
}

/* =====================================================================
 * 具象实现：runOnce（不建线程，手动排空当前队列；供嵌入既有帧循环）
 *   注意：若循环同时正在自身线程上运行，任务可能被两个线程并发执行 ——
 *   「嵌入既有循环」的用法就是【不】调 start()，二者只取其一。
 * ===================================================================== */
static int eventLoopImplRunOnce(void* instance)
{
    EventLoopImpl* self;
    int            executed = 0;

    ASSERT_NOT_NULL(instance, "eventLoop runOnce: instance is null", -1);

    self = (EventLoopImpl*)instance;

    for (;;) {
        EventLoopTask task;
        void*         taskParam;

        eventLoopLock(self);
        if (self->count <= 0) {
            eventLoopUnlock(self);
            break;
        }
        task      = self->tasks[self->head];
        taskParam = self->params[self->head];
        self->head = (self->head + 1) % EVENT_LOOP_TASK_CAP;
        self->count--;
        eventLoopUnlock(self);

        /* 锁外执行（与 worker 同口径）：一个长任务不得挡住 post() */
        if (task != NULL) {
            task(taskParam);
        }
        executed++;
    }

    return executed;
}

/* =====================================================================
 * 具象实现：isRun
 * ===================================================================== */
static int eventLoopImplIsRun(void* instance)
{
    if (instance == NULL) {
        return 0;
    }

    return EVENT_LOOP_ATOMIC_LOAD(&((EventLoopImpl*)instance)->isRun);
}

/* =====================================================================
 * 具象实现：deleteInstance（析构具体实例；抽象包装器由 DeleteEventLoop 释放）
 *   顺序：先确认 worker 已退出（有界等待；超时才经借用的 ThreadObject
 *   强停 —— 冻结契约承诺的「兜底强停」）→ 销毁互斥量 → 释放实例本体。
 * ===================================================================== */
static void eventLoopImplDeleteInstance(void* instance)
{
    EventLoopImpl* self;

    if (instance == NULL) {
        return;
    }
    self = (EventLoopImpl*)instance;

    if (EVENT_LOOP_ATOMIC_LOAD(&self->isRun) != 0) {
        EVENT_LOOP_ATOMIC_STORE(&self->isQuit, 1);
        if (eventLoopWaitFlagZero(&self->isRun, EVENT_LOOP_STOP_TIMEOUT_MS) != 0) {
            log_warn("eventLoop deleteInstance: timed out waiting for the loop to exit "
                     "(timeout=%d ms, loop name=\"%s\") —— force-stopping the borrowed "
                     "ThreadObject now (hard kill; the worker may have died holding the "
                     "queue mutex)",
                     EVENT_LOOP_STOP_TIMEOUT_MS, self->name);

            /* 兜底强停：ThreadObject::stop() 是硬杀（TerminateThread /
             * pthread_cancel）。不到「等待超时」不走此路；走下去是为了
             * 避免「释放实例本体」与「仍在运行的 worker」构成 use-after-free。 */
            if (self->thread != NULL && self->thread->interfaces.stop != NULL) {
                (void)self->thread->interfaces.stop(self->thread->instance);
                log_error("eventLoop deleteInstance: the loop was FORCE-KILLED after the "
                          "bounded wait timed out (loop name=\"%s\") —— cooperative shutdown "
                          "did not happen in time; investigate why the worker was stalled",
                          self->name);
            }
            self->thread = NULL;
        }
    }

    /* 生产者 / 消费者均已不存在 ⇒ 可安全销毁互斥量（DeletePlatformMutex
     * 内部会释放抽象对象并把本指针置 NULL，防悬空）。 */
    if (self->mutex != NULL) {
        DeletePlatformMutex(&self->mutex);
    }

    eventLoopFree(self);
}

/* =====================================================================
 * worker：event_loop 所属线程的线程体（由 ThreadObject::start 注入）
 *   - 经宏置 isRun = 1（release 写，与 stop() 的 acquire 读配对）；
 *   - while (isQuit == 0)（acquire 读）：持锁取一个任务 → 解锁 → 【锁外】执行；
 *     队列空则解锁后 sleepMs 让出 CPU（无事件/信号量原语可用）；
 *   - 退出前经宏置 isRun = 0，并顺手经宏把 isQuit 复位（便于下次 start）。
 * ===================================================================== */
static void eventLoopWorker(void* param)
{
    EventLoopImpl* self = (EventLoopImpl*)param;

    if (self == NULL) {
        return;
    }

    EVENT_LOOP_ATOMIC_STORE(&self->isRun, 1);

    while (EVENT_LOOP_ATOMIC_LOAD(&self->isQuit) == 0) {
        EventLoopTask task;
        void*         taskParam;

        eventLoopLock(self);
        if (self->count > 0) {
            task      = self->tasks[self->head];
            taskParam = self->params[self->head];
            self->head = (self->head + 1) % EVENT_LOOP_TASK_CAP;
            self->count--;
            eventLoopUnlock(self);

            /* 绝不在持锁状态下执行任务：否则 post() 会被一个长任务长时间挡住 */
            if (task != NULL) {
                task(taskParam);
            }
        } else {
            eventLoopUnlock(self);
            eventLoopSleepMs(EVENT_LOOP_IDLE_SLEEP_MS);
        }
    }

    EVENT_LOOP_ATOMIC_STORE(&self->isRun, 0);
    EVENT_LOOP_ATOMIC_STORE(&self->isQuit, 0);   /* 复位，便于下次 start() */
}

/* =====================================================================
 * 虚表装配：宏 + C11 指定初始化器集中返回（deleteInstance 必须最后一项）。
 *   【必须 6 槽全装配】漏装任何一项都会在虚表里留下未初始化的垃圾函数
 *   指针（本函数返回的是栈上局部结构体的值，不作零填充），一旦被调用
 *   即崩溃。
 * ===================================================================== */
#define _RETURN_EVENT_LOOP_INTERFACE                             \
    EventLoopInterface interfaces = {                            \
        .post           = eventLoopImplPost,                     \
        .start          = eventLoopImplStart,                    \
        .stop           = eventLoopImplStop,                     \
        .runOnce        = eventLoopImplRunOnce,                 \
        .isRun          = eventLoopImplIsRun,                    \
        .deleteInstance = eventLoopImplDeleteInstance           \
    };                                                           \
    return interfaces;

/* 通过宏集中返回本具象的虚表（供具体构造装配使用） */
static EventLoopInterface eventLoopInterface(void)
{
    _RETURN_EVENT_LOOP_INTERFACE
}

/* =====================================================================
 * 具体构造：NewEventLoopDefault(const char* name)
 *   - 参数自定义：name 仅作诊断用（可传 NULL ⇒ 空串；过长则截断 +
 *     log_warn —— 循环名不参与任何逻辑判断，截断是安全的）；
 *   - 内部【必须】调用抽象构造 NewEventLoop 完成装配，严禁手工给抽象
 *     对象写 instance / interfaces；
 *   - 失败路径逆序回收（互斥量 → 实例本体），绝不部分构造成功。
 *
 * 说明：冻结头 event_loop.h 只声明抽象层三件套（与 translate_unit.h
 * 只声明 NewTranslateUnit、具象构造放 translate_unit_echo.h 的仓库惯例
 * 一致）；本具体构造是对外使用的入口，其声明应由编排方补在冻结头
 * （或具象头 event_loop_default.h）中，见交付报告「待登记」项。
 * ===================================================================== */
EventLoop* NewEventLoopDefault(const char* name)
{
    EventLoopImpl* self;
    EventLoop*     loop;
    size_t         nameLen;

    self = (EventLoopImpl*)eventLoopMalloc(sizeof(EventLoopImpl));
    ASSERT_NOT_NULL(self, "NewEventLoopDefault: eventLoopMalloc failed", NULL);
    memset(self, 0, sizeof(EventLoopImpl));

    if (name == NULL) {
        log_warn("NewEventLoopDefault: name is null (diagnostic only) —— using an empty name");
        self->name[0] = '\0';
    } else {
        nameLen = strlen(name);
        if (nameLen >= (size_t)EVENT_LOOP_NAME_CAP) {
            log_warn("NewEventLoopDefault: name is too long (actual=%u chars, cap=%d) —— "
                     "truncated (the loop name is diagnostic only; truncation is safe)",
                     (unsigned)nameLen, EVENT_LOOP_NAME_CAP);
            nameLen = (size_t)EVENT_LOOP_NAME_CAP - 1;
        }
        memcpy(self->name, name, nameLen);
        self->name[nameLen] = '\0';
    }

    /* 环形队列空栈起步；线程句柄待 start() 注入（借用）。
     * isQuit / isRun 亦经宏清零（构造期尚无并发，与裸写等价，统一口径）。 */
    self->thread = NULL;
    self->head   = 0;
    self->tail   = 0;
    self->count  = 0;
    EVENT_LOOP_ATOMIC_STORE(&self->isQuit, 0);
    EVENT_LOOP_ATOMIC_STORE(&self->isRun, 0);

    /* 保护任务队列的互斥量（复用 platform_sync 能力；生产者/消费者各一个线程） */
    self->mutex = NewPlatformMutexForPlatform();
    if (self->mutex == NULL) {
        log_error("NewEventLoopDefault: create queue mutex failed "
                  "(NewPlatformMutexForPlatform returned null; loop name=\"%s\")", self->name);
        eventLoopFree(self);
        return NULL;
    }

    /* 抽象构造完成装配（禁止手工赋值 instance/interfaces） */
    loop = NewEventLoop(self, eventLoopInterface());
    if (loop == NULL) {
        DeletePlatformMutex(&self->mutex);
        eventLoopFree(self);
        return NULL;
    }

    return loop;
}
