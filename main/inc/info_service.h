#ifndef _INFO_SERVICE_H
#define _INFO_SERVICE_H

/* ============================================================================
 * info_service —— 信息更新服务（debug 定时回传 / release 被动读取）
 *
 * 单一职责：
 *   把「当前加速度快照」按需回传到调试串口。
 *     - debug 构建：内部起一个定时线程，按 pushPeriodMs 周期把回传任务投递到
 *       注入的 EventLoop（UART 通讯线程），串口输出因此只发生在一个线程里；
 *     - release 构建：不建线程、不定时器，只有上层主动调用 InfoServiceRequestPush
 *       时才回传一次（被动读取），避免周期唤醒与无用代码体积。
 *
 * 依赖反转：
 *   1. 本模块不包含 accel_service —— 快照由 InfoSampleProvider 注入；
 *   2. 回传动作只在注入的 EventLoop 线程内执行（post），本模块不直接依赖 UART。
 *
 * 所有权约定：
 *   - EventLoop 与 providerContext 均为借用，本模块不负责销毁；
 *   - 本模块内部创建的定时线程与定时器由本模块自己回收。
 * ========================================================================== */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 本项目 C 头自带 extern "C" 包裹，此处嵌套合法且幂等；
 * 置于包裹内可保证它们间接引入的头（断言/日志等）同样以 C 链接展开。 */
#include "app_types.h"    /* AccelSample / APP_INFO_PUSH_PERIOD_MS */
#include "event_loop.h"   /* EventLoop / EventLoopTask */

typedef struct InfoService InfoService;

/* 取快照的注入点：由装配区包装 AccelServiceGet（本模块不直接依赖 accel_service）。
 * outSample 由本模块提供，调用前已清零；实现方只需填入自己关心的字段。 */
typedef void (*InfoSampleProvider)(void* context, AccelSample* outSample);

/* 构造配置：字段在 NewInfoService 内被拷贝，构造完成后调用方可释放自己的副本。 */
typedef struct InfoServiceConfig {
    EventLoop*         loop;             /* 必需：回传任务在此对象所属线程执行（UART 通讯线程） */
    InfoSampleProvider provider;         /* 必需：取当前快照 */
    void*              providerContext;  /* 可选：原样回传给 provider 的上下文 */
    uint32_t           pushPeriodMs;     /* debug 回传周期（毫秒）；0 或 1 回落到默认周期 */
} InfoServiceConfig;

/* 具体构造：config 非法（NULL，或 loop / provider 为空）返回 NULL。
 * debug 构建下同时创建定时线程与周期定时器，创建失败返回 NULL 并回收已建资源。
 * 释放用 DeleteInfoService。 */
InfoService* NewInfoService(const InfoServiceConfig* config);

/* 析构：停止并回收定时器与定时线程，释放自身并把 *pService 置 NULL 防悬空。
 * 注入的 loop / providerContext 不属于本模块，一律不释放。
 * pService 为 NULL 或 *pService 为 NULL 时直接返回。 */
void DeleteInfoService(InfoService** pService);

/* 启动信息更新：
 *     - debug 构建：启动周期定时器（pushPeriodMs）；
 *     - release 构建：空实现，直接返回 0（装配代码无需按构建模式分支）。
 * 返回：0 成功；-1 参数非法或定时器启动失败；-2 定时器不可用（尚未装配成功）。 */
int  InfoServiceStart(InfoService* service);

/* 手动触发一次回传（debug / release 均可用）：把回传任务 post 到注入的 EventLoop，
 * 真正的打印发生在 loop 所属线程。
 * 返回：0 成功；-1 参数非法；-2 投递失败（队列已满 / 循环未运行）。 */
int  InfoServiceRequestPush(InfoService* service);

#ifdef __cplusplus
}
#endif

#endif /* _INFO_SERVICE_H */
