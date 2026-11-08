#ifndef _TIMESTAMP_FREERTOS_H
#define _TIMESTAMP_FREERTOS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* FreeRTOS 平台时间戳实现。
 *
 * 语义说明（与 timestamp.h 的 Unix 纪元语义不同）：
 *   本模块的返回值以「系统启动」为基准，即调度器/tick 计数的起点（tick == 0 时刻），
 *   并非 Unix 纪元（1970-01-01 00:00:00 UTC）。因此返回的是「自启动以来经过的时间量」，
 *   而非可换算成日历的绝对时间。需要日历/绝对时间时，由上层配合 RTC 处理。
 *
 * 精度说明：
 *   - 毫秒：由 xTaskGetTickCount() 按 configTICK_RATE_HZ 换算，精度受 tick 粒度限制。
 *   - 微秒：ESP-IDF 下使用 esp_timer_get_time() 高精度计时器（本身即自启动以来的微秒数）；
 *           通用 FreeRTOS 下退回 tick 换算，精度受 tick 粒度限制（例如 100Hz -> 10ms 粒度）。
 *   - 纳秒：由微秒值 ×1000 得到，并非硬件纳秒级，实际精度同样受 tick / esp_timer 粒度限制。
 *
 * 三个函数均无全局初始化状态依赖、无内存分配、纯只读，可重入。
 */

/**
 * 获取自系统启动（tick 计数起点）以来的毫秒数。
 * @return 毫秒计数；精度受 tick 粒度限制。
 */
uint64_t getTimestampFreertosMs(void);

/**
 * 获取自系统启动以来的微秒数。
 * @return 微秒计数；ESP-IDF 下为 esp_timer 高精度值，通用 FreeRTOS 下精度受 tick 粒度限制。
 */
uint64_t getTimestampFreertosUs(void);

/**
 * 获取自系统启动以来的纳秒数（由微秒值 ×1000 得到，非硬件纳秒）。
 * @return 纳秒计数；实际精度受 tick / esp_timer 粒度限制。
 */
uint64_t getTimestampFreertosNs(void);

#ifdef __cplusplus
}
#endif

#endif /* _TIMESTAMP_FREERTOS_H */
