#include "platform_timestamp.h"

/* =====================================================================
 * platform_timestamp —— 跨平台「时间戳」能力实现（非对象式）
 *
 * 仅按平台选定一组函数指针并返回给调用方；本模块自身不分配内存、
 * 不持有任何堆对象，也不引入任何静态可变状态（无全局变量）。
 *
 * 文件结构按「宏决定导入哪个模块头文件、创建哪个函数」组织：
 *   - 能力关闭(PLATFORM_USE_TIMESTAMP==0)：不生成任何函数；
 *   - FreeRTOS                          ：导入 timestamp_freertos.h；
 *   - Windows / Linux / macOS           ：导入 timestamp.h。
 * 两来源头文件互斥，严禁同时包含。
 *
 * 语义差异提示（详见 platform_timestamp.h）：
 *   - Windows / Linux / macOS 返回 Unix 纪元绝对时间；
 *   - FreeRTOS 返回自系统启动以来的相对时间（非 Unix 纪元）。
 * ===================================================================== */

#if !PLATFORM_USE_TIMESTAMP

/* 能力关闭：不生成任何函数。
 * 头文件中的 struct PlatformTimestampInterface typedef 仍然存在，
 * 因此本编译单元非空，无需额外占位。 */

#elif defined(PLATFORM_FREERTOS)

#include "timestamp_freertos.h"

PlatformTimestampInterface NewPlatformTimestamp(void)
{
    /* FreeRTOS：返回自系统启动（tick 计数起点）以来的时间计数，
     * 非 Unix 纪元绝对时间。 */
    PlatformTimestampInterface ts;
    ts.milliseconds = getTimestampFreertosMs;
    ts.microseconds = getTimestampFreertosUs;
    ts.nanoseconds  = getTimestampFreertosNs;
    return ts;
}

#else   /* WINDOWS / LINUX / MACOS 共用既有 timestamp 模块 */

#include "timestamp.h"

PlatformTimestampInterface NewPlatformTimestamp(void)
{
    /* Windows / Linux / macOS：返回 Unix 纪元（1970-01-01 UTC）以来的
     * 绝对时间计数。 */
    PlatformTimestampInterface ts;
    ts.milliseconds = get_timestamp_ms;
    ts.microseconds = get_timestamp_us;
    ts.nanoseconds  = get_timestamp_ns;
    return ts;
}

#endif /* 平台分支 */
