#ifndef _PLATFORM_TIMESTAMP_H
#define _PLATFORM_TIMESTAMP_H

#include <stdint.h>

/* 本头会被 C++ 编译单元（Qt 具象层、demo）包含，故自带 extern "C" 包裹，
   保证 C++ 单元看到的是 C 名字修饰（避免链接期 undefined reference）。
   标准库头（<stdint.h>）留在包裹之外；本项目的 C 头（platform.h）与
   下方全部 typedef / 函数声明一并放进包裹之内。platform.h 自身亦带包裹，
   此处出现的「extern "C" 嵌套」在 C++ 中是合法的。 */
#ifdef __cplusplus
extern "C" {
#endif

#include "platform.h"

/* =====================================================================
 * platform_timestamp —— 跨平台「时间戳」能力的对外契约（非对象式）
 *
 * 本模块只提供「一组按平台选定的函数指针」，不做任何内存分配、
 * 不持有任何静态可变状态。调用方通过 NewPlatformTimestamp() 取得
 * 当前平台的函数集后即可按统一签名取时间。
 *
 * ---------------------------------------------------------------------
 * 关键契约：语义差异（消费方务必注意）
 * ---------------------------------------------------------------------
 *   - Windows / Linux / macOS：
 *       复用既有 timestamp 模块，返回值为 Unix 纪元（1970-01-01 00:00:00
 *       UTC）以来的绝对时间；可换算成日历时间，可跨平台做绝对时间比较。
 *
 *   - FreeRTOS：
 *       返回值为「自系统启动（tick 计数起点，即 tick == 0 时刻）以来」
 *       经过的时间量，并非 Unix 纪元；它不是可换算成日历的绝对时间。
 *       需要日历/绝对时间时，须由上层配合 RTC 处理。
 *
 *   两者语义不同，消费方不得跨平台把返回值当作同一种绝对时间做混算
 *   （例如把 FreeRTOS 的启动相对量与 Unix 纪元时间相减）。
 *
 * ---------------------------------------------------------------------
 * 精度说明
 * ---------------------------------------------------------------------
 *   - 毫秒/微秒/纳秒的可用精度由所选平台的具体实现决定
 *     （Windows 为 QPC；Linux/macOS 为 CLOCK_REALTIME；FreeRTOS 受
 *     tick 粒度或 esp_timer 精度限制，纳秒非硬件级）。
 *
 * 能力裁剪：
 *   - 当 PLATFORM_USE_TIMESTAMP == 0 时，本头不声明 NewPlatformTimestamp，
 *     实现侧同样不生成该函数（见 platform_timestamp.c）。
 * ===================================================================== */

/* 跨平台时间戳接口（非对象式：一组按平台选定的函数指针）。
 * 三个函数均无参数，返回对应单位的时间计数；语义见上方契约说明。 */
typedef struct PlatformTimestampInterface{
    uint64_t (*milliseconds)(void);
    uint64_t (*microseconds)(void);
    uint64_t (*nanoseconds)(void);
} PlatformTimestampInterface;

#if PLATFORM_USE_TIMESTAMP
/* 返回当前平台的时间戳函数集 */
PlatformTimestampInterface NewPlatformTimestamp(void);
#endif /* PLATFORM_USE_TIMESTAMP */

#ifdef __cplusplus
}
#endif

#endif /* _PLATFORM_TIMESTAMP_H */
