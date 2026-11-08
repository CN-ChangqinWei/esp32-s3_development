#include "platform_thread.h"

/* =====================================================================
 * platform_thread —— 跨平台「线程对象」能力实现（对象式工厂）
 *
 * 结构：以「能力开关 + 平台宏」组织为互斥分支：能力关闭则不生成任何函数；
 *       开启时每个平台分支只包含「当前平台」的具象头文件（严禁同一编译
 *       单元同时包含多个平台具象头，保证平台隔离），并且只在命中分支内
 *       定义 NewPlatformThreadObject。
 * 内存：本层不做任何分配/释放，全部委托给具象 NewThreadObjectXxx。
 * 健壮性：先判空；int->size_t 的 stackSize 先判 <=0 再显式转换，
 *         避免负值被强转成巨大的栈请求。
 * ===================================================================== */

#if !PLATFORM_USE_THREAD_OBJECT

/* 能力关闭：不生成任何函数。
 * platform_thread.h 中的 PlatformThreadConfig typedef 仍然存在，
 * 因此本编译单元非空，无需额外占位，也不会产生未用变量告警。 */

#elif defined(PLATFORM_WINDOWS)

#include "thread_object_windows.h"
#include <string.h>

/* Windows 具象：stackSize 为字节；safe 由本层置 NULL（不使用安全句柄）。 */
ThreadObject* NewPlatformThreadObject(PlatformThreadConfig* config)
{
    ThreadObjectWindowsConfig cfg;

    ASSERT_NOT_NULL(config, "NewPlatformThreadObject: config is null", NULL);

    memset(&cfg, 0, sizeof(cfg));
    cfg.safe      = NULL;
    cfg.stackSize = config->stackSize;
    cfg.workFunc  = config->workFunc;
    cfg.workParam = config->workParam;

    return NewThreadObjectWindows(&cfg);
}

#elif defined(PLATFORM_LINUX)

#include "thread_object_linux.h"
#include <string.h>

/* Linux 具象：stackSize 为字节（size_t）；int->size_t 需先判 <=0 归 0，
 * 避免负值被强转成巨大的栈请求。priority/name 该平台忽略。 */
ThreadObject* NewPlatformThreadObject(PlatformThreadConfig* config)
{
    ThreadObjectLinuxConfig cfg;

    ASSERT_NOT_NULL(config, "NewPlatformThreadObject: config is null", NULL);

    memset(&cfg, 0, sizeof(cfg));
    cfg.stackSize = (size_t)(config->stackSize > 0 ? config->stackSize : 0);
    cfg.workFunc  = config->workFunc;
    cfg.workParam = config->workParam;

    return NewThreadObjectLinux(&cfg);
}

#elif defined(PLATFORM_MACOS)

#include "thread_object_macos.h"
#include <string.h>

/* macOS 具象：与 Linux 同为 POSIX，stackSize 为字节（size_t），判 <=0 归 0。 */
ThreadObject* NewPlatformThreadObject(PlatformThreadConfig* config)
{
    ThreadObjectMacosConfig cfg;

    ASSERT_NOT_NULL(config, "NewPlatformThreadObject: config is null", NULL);

    memset(&cfg, 0, sizeof(cfg));
    cfg.stackSize = (size_t)(config->stackSize > 0 ? config->stackSize : 0);
    cfg.workFunc  = config->workFunc;
    cfg.workParam = config->workParam;

    return NewThreadObjectMacos(&cfg);
}

#elif defined(PLATFORM_FREERTOS)

#include "thread_object_freertos.h"
#include <string.h>

/* FreeRTOS 具象：stackSize -> stackDepth（单位「字」，<=0 由具象取默认）；
 * priority / name 直接透传。 */
ThreadObject* NewPlatformThreadObject(PlatformThreadConfig* config)
{
    ThreadObjectFreertosConfig cfg;

    ASSERT_NOT_NULL(config, "NewPlatformThreadObject: config is null", NULL);

    memset(&cfg, 0, sizeof(cfg));
    cfg.stackDepth = config->stackSize;
    cfg.priority   = config->priority;
    cfg.name       = config->name;
    cfg.workFunc   = config->workFunc;
    cfg.workParam  = config->workParam;

    return NewThreadObjectFreertos(&cfg);
}

#endif /* PLATFORM_USE_THREAD_OBJECT / 平台互斥分支 */
