#ifndef _EVENT_LOOP_H_
#define _EVENT_LOOP_H_

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 【关键】assert_marco.h 会引入 log.h，而 libs/log/inc/log.h 自身没有 extern "C" 包裹，
 * 必须在 extern "C" 内首次包含它。三个依赖头各自已带 extern "C" 包裹，嵌套合法。 */
#include "assert_marco.h"
#include "thread_object.h"
#include "platform_sync.h"
#include "platform_sleep.h"
#include "platform_timestamp.h"

/* ---------------------------------------------------------------------
 * 跨线程标志的原子访问宏（isQuit / isRun）
 *
 *   背景：isQuit / isRun 由「外部线程（stop / start / isRun 查询）」与
 *   「worker 线程」并发读写；volatile 不提供原子性也不提供内存序保证
 *   （C 内存模型下属数据竞争 = UB），必须经原子内建访问。
 *
 *   为何不用 <stdatomic.h> + _Atomic：
 *     本工具链（MinGW GCC 13.1 + C++17）下 <stdatomic.h> 的 C++ 版只在
 *     C++23 起定义 _Atomic，C 版又用 C 专属限定符 C++ 不接受 ⇒ 编译失败。
 *     __atomic_*_n 内建是唯一可编译路径（已被 demo 与构建验证证实）。
 *
 *   为何用显式 RELEASE / ACQUIRE：
 *     写方（stop / start / worker 退出）release，读方（worker 主循环 /
 *     isRun 查询 / 有界轮询）acquire，配对保证「标志写入」与「标志读取」
 *     之间的顺序可见；显式标注内存序也便于审查。
 *
 *   非 GCC / Clang 退化为 volatile 普通读写：不保证原子性，但不劣于
 *   本次改动之前的裸 volatile 访问。
 * ------------------------------------------------------------------- */
#if defined(__GNUC__) || defined(__clang__)
#define EVENT_LOOP_ATOMIC_STORE(p, v) __atomic_store_n((p), (v), __ATOMIC_RELEASE)
#define EVENT_LOOP_ATOMIC_LOAD(p)     __atomic_load_n((p), __ATOMIC_ACQUIRE)
#else
#define EVENT_LOOP_ATOMIC_STORE(p, v) (*(p) = (v))
#define EVENT_LOOP_ATOMIC_LOAD(p)     (*(p))
#endif

#define EVENT_LOOP_TASK_CAP        256   /* 队列槽位数；建议为 2 的幂 */
#define EVENT_LOOP_NAME_CAP         32   /* 循环名容量（诊断用） */
#define EVENT_LOOP_IDLE_SLEEP_MS     2   /* 队列空时的让出间隔（毫秒） */
#define EVENT_LOOP_STOP_TIMEOUT_MS 2000  /* stop() 有界等待上限（毫秒） */

/* 任务原型：在 event_loop 所属线程内被同步调用 */
typedef void (*EventLoopTask)(void* param);

typedef struct EventLoopInterface{
    /* 入队一个任务。0 = 成功；
     * <0 = instance 为 NULL / task 为 NULL / 队列已满（丢弃并 log_warn）。 */
    int  (*post)(void* instance, EventLoopTask task, void* param);

    /* 在【外部注入的 ThreadObject】上启动循环，worker 由本模块内部提供。
     * 0 = 成功；<0 = instance/threadObj 为 NULL / 循环已在运行 / 线程启动失败。
     * threadObj 的所有权【不转移】：本模块只借用，不负责销毁它。 */
    int  (*start)(void* instance, struct ThreadObject* threadObj);

    /* 请求退出并【有界等待】循环真正结束。0 = 已退出；
     * <0 = instance 为 NULL / 等待超时（此时循环可能仍在跑，deleteInstance 会兜底强停）。 */
    int  (*stop)(void* instance);

    /* 不建线程，手动执行当前队列中【全部】任务后返回执行的任务数；
     * 供把 event_loop 嵌入既有循环（如 imgui 帧循环）。<0 = instance 为 NULL。 */
    int  (*runOnce)(void* instance);

    int  (*isRun)(void* instance);   /* 1 = 循环正在跑；0 = 否/NULL */
    void (*deleteInstance)(void* instance);   /* 析构（必需项），保持最后一项 */
} EventLoopInterface;

typedef struct EventLoop{
    void*             instance;      /* 指向具体实现实例（多态核心） */
    EventLoopInterface interfaces;   /* 虚表，字段名固定为 interfaces */
} EventLoop;

/* 本模块的分配器：可被外部整体替换（依赖反转），默认落到 NewPlatformMemory() */
extern void* (*eventLoopMalloc)(size_t size);
extern void  (*eventLoopFree)(void* p);

EventLoop* NewEventLoop(void* instance, EventLoopInterface interfaces);   /* 抽象构造：参数表固定，勿改 */
int        InitEventLoop(EventLoop* loop, void* instance, EventLoopInterface interfaces); /* 就地注入 */
void       DeleteEventLoop(EventLoop** pLoop);   /* 先 deleteInstance，再释放抽象对象，最后置 NULL */

/* 默认具象的具体构造（本模块目前唯一具象，故直接声明在此，避免为单个具象另开头文件；
 * 若日后出现第二个具象，应拆出 event_loop_<name>.h 并把本声明迁过去）。
 * name 仅用于诊断日志：NULL / 空串 ⇒ 空名；超长截断并 log_warn。
 * 内部调用抽象构造 NewEventLoop 完成装配；释放用 DeleteEventLoop。 */
EventLoop* NewEventLoopDefault(const char* name);

#ifdef __cplusplus
}
#endif
#endif /* _EVENT_LOOP_H_ */
