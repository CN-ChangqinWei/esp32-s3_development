#include "timestamp_freertos.h"

/* FreeRTOS 头引入顺序必须为：FreeRTOS.h 在前，task.h 在后。 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if defined(ESP_PLATFORM)
/* ESP-IDF 高精度计时器：esp_timer_get_time() 返回自启动以来的微秒数（int64_t）。 */
#include <esp_timer.h>
#endif

/**
 * 获取自系统启动（tick 计数起点）以来的毫秒数。
 *
 * 换算：ticks * 1000 / configTICK_RATE_HZ。
 * 采用 64 位中间量（先乘后除），避免 32 位 tick 在乘法阶段溢出；
 * configTICK_RATE_HZ（通常为 1000/100）被提升为 uint64_t 参与运算，确保整除前的类型提升正确。
 * 精度受 tick 粒度限制。
 */
uint64_t getTimestampFreertosMs(void)
{
    uint64_t ticks = (uint64_t)xTaskGetTickCount();
    return ticks * 1000ULL / (uint64_t)configTICK_RATE_HZ;
}

/**
 * 获取自系统启动以来的微秒数。
 *
 * - ESP-IDF（ESP_PLATFORM）：直接使用 esp_timer_get_time()，其值本身即「自启动以来的微秒数」，
 *   精度远高于 tick 换算。
 * - 通用 FreeRTOS：退回 tick 换算 ticks * 1000000 / configTICK_RATE_HZ。
 *   此时精度受 tick 粒度限制（例如 configTICK_RATE_HZ == 100 时，粒度为 10ms；
 *   即函数返回的微秒值只会在 10ms 的阶梯上变化）。
 *
 * 同样使用 64 位中间量，避免溢出。
 */
uint64_t getTimestampFreertosUs(void)
{
#if defined(ESP_PLATFORM)
    /* esp_timer_get_time() 返回 int64_t 微秒数，转换为无符号展示。 */
    return (uint64_t)esp_timer_get_time();
#else
    uint64_t ticks = (uint64_t)xTaskGetTickCount();
    return ticks * 1000000ULL / (uint64_t)configTICK_RATE_HZ;
#endif
}

/**
 * 获取自系统启动以来的纳秒数。
 *
 * 由微秒值 ×1000 得到。注意这并非硬件纳秒级计时：
 * 实际分辨率取决于底层微秒来源（ESP-IDF 的 esp_timer 或 tick 换算），
 * 通用 FreeRTOS 下精度受 tick 粒度限制（例如 100Hz -> 10ms，即纳秒值以 10,000,000ns 为阶梯变化）。
 * 64 位运算下，即使 tick 达到 2^32 量级也不会溢出。
 */
uint64_t getTimestampFreertosNs(void)
{
    uint64_t micros = getTimestampFreertosUs();
    return micros * 1000ULL;
}
