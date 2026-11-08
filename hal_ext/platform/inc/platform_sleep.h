#ifndef _PLATFORM_SLEEP_H
#define _PLATFORM_SLEEP_H

#include <stdint.h>

/* 本头会被 C++ 编译单元（Qt 具象层 / demo）包含，故自带 extern "C" 包裹，
   且把本项目 C 头 platform.h 的包含置于包裹**之内**：这样无论本头与平台聚合头
   谁先被包含，platform.h 及其内部的全部 platform_*.h 都以 C 链接展开，
   消除「谁先包含」对语言链接的隐式耦合。platform.h 自身亦带包裹，
   此处出现的「extern "C" 嵌套」在 C++ 中是合法且幂等的。
   标准库头（<stdint.h>）留在包裹之外，与 platform_memory.h / platform_timestamp.h 一致。 */
#ifdef __cplusplus
extern "C" {
#endif

#include "platform.h"

/* =====================================================================
 * platform_sleep —— 跨平台「睡眠」能力的对外契约（非对象式）
 *
 * 本模块只提供「一组按平台选定的函数指针」，不做任何内存分配、
 * 不持有任何静态可变状态（无全局变量）。调用方通过 NewPlatformSleep()
 * 取得当前平台的函数集后即可按统一签名让当前执行体挂起。
 *
 * ---------------------------------------------------------------------
 * 关键契约：语义差异（消费方务必注意）
 * ---------------------------------------------------------------------
 *   - Windows：
 *       sleepMs 走系统级 Sleep()（毫秒级，可被抢占）；
 *       sleepUs 当 us >= 1000 时退化为毫秒级 Sleep()，否则以
 *       QueryPerformanceCounter 忙等（Windows 无系统级微秒 sleep 原语）。
 *
 *   - Linux / macOS：
 *       sleepMs 走 nanosleep（可安全处理较大毫秒值）；
 *       sleepUs 走 usleep（POSIX 微秒级挂起）。
 *
 *   - FreeRTOS：
 *       sleepMs 走 vTaskDelay(pdMS_TO_TICKS(ms))（受 tick 粒度限制）；
 *       sleepUs 在 ESP-IDF 下走 esp_rom_delay_us()，否则为忙等循环
 *       （FreeRTOS 无通用微秒级睡眠原语），二者均为忙等语义、不释放 CPU。
 *
 *   消费方不得跨平台假设「us 级睡眠一定有系统级精度」或「一定会让出 CPU」。
 *
 * 能力裁剪：
 *   - 当 PLATFORM_USE_SLEEP == 0 时，本头不声明 NewPlatformSleep，
 *     实现侧同样不生成该函数（见 platform_sleep.c）。
 * ===================================================================== */

/* 跨平台睡眠接口（非对象式：一组按平台选定的函数指针）。
 *   sleepMs(ms) : 挂起至少 ms 毫秒；ms == 0 时立即返回。
 *   sleepUs(us) : 挂起至少 us 微秒；us == 0 时立即返回。
 * 两个函数均无返回值，也不分配任何内存、不持有任何状态。 */
typedef struct PlatformSleepInterface{
    void (*sleepMs)(uint32_t ms);
    void (*sleepUs)(uint32_t us);
} PlatformSleepInterface;

#if PLATFORM_USE_SLEEP
/* 返回当前平台的睡眠函数集；能力关闭(PLATFORM_USE_SLEEP==0)时不生成此函数 */
PlatformSleepInterface NewPlatformSleep(void);
#endif /* PLATFORM_USE_SLEEP */

#ifdef __cplusplus
}
#endif

#endif /* _PLATFORM_SLEEP_H */
