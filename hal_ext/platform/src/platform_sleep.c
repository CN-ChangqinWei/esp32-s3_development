#include "platform_sleep.h"

/* =====================================================================
 * platform_sleep —— 跨平台「睡眠」能力实现（非对象式）
 *
 * 仅按平台选定一组函数指针并返回给调用方；本模块自身不分配任何内存、
 * 不持有任何堆对象，也不引入任何静态可变状态（无全局变量）。
 *
 * 文件结构按「能力开关 + 平台宏」互斥分支组织：
 *   - 能力关闭(PLATFORM_USE_SLEEP==0)：不生成任何函数；
 *   - Windows                          ：导入 <windows.h>；
 *   - Linux / macOS                    ：导入 <unistd.h> + <time.h>；
 *   - FreeRTOS                         ：导入 freertos/FreeRTOS.h + freertos/task.h
 *                                         （ESP-IDF 下额外导入 esp_rom_sys.h）。
 * 各平台头文件互斥，严禁同一编译单元同时包含多个平台的头文件。
 *
 * 健壮性：入参为 0 时立即返回，不产生死循环/无限阻塞；微秒忙等在 us==0
 * 时不进入循环。本能力不做任何堆分配（无 malloc/free/calloc/realloc）。
 * ===================================================================== */

#if !PLATFORM_USE_SLEEP

/* 能力关闭：不生成任何函数。
 * 头文件中的 struct PlatformSleepInterface typedef 仍然存在，
 * 因此本编译单元非空，无需额外占位。 */

#elif defined(PLATFORM_WINDOWS)

#include <windows.h>

static void windowsSleepMs(uint32_t ms)
{
    if (ms == 0) {
        return;
    }
    Sleep((DWORD)ms);
}

static void windowsSleepUs(uint32_t us)
{
    LARGE_INTEGER freq;
    LARGE_INTEGER start;
    LARGE_INTEGER now;
    long long elapsedUs;

    if (us == 0) {
        return;
    }

    /* Windows 无系统级微秒 sleep 原语：
     * us >= 1000 时退化为毫秒级 Sleep；不足 1ms 的部分只能靠忙等。 */
    if (us >= 1000u) {
        Sleep((DWORD)(us / 1000u));
        return;
    }

    if (QueryPerformanceFrequency(&freq) == 0 || freq.QuadPart <= 0) {
        /* 高精度计数器不可用：极短的 us 无法精确等待，直接返回。 */
        return;
    }
    QueryPerformanceCounter(&start);
    do {
        QueryPerformanceCounter(&now);
        elapsedUs = (long long)((now.QuadPart - start.QuadPart) * 1000000LL / freq.QuadPart);
    } while (elapsedUs < (long long)us);
}

PlatformSleepInterface NewPlatformSleep(void)
{
    PlatformSleepInterface sleep;
    sleep.sleepMs = windowsSleepMs;
    sleep.sleepUs = windowsSleepUs;
    return sleep;
}

#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)

#include <unistd.h>
#include <time.h>

static void posixSleepMs(uint32_t ms)
{
    struct timespec req;

    if (ms == 0) {
        return;
    }

    /* 使用 nanosleep 以安全处理较大的毫秒值（避免 usleep 上限问题）。 */
    req.tv_sec  = (time_t)(ms / 1000u);
    req.tv_nsec = (long)((ms % 1000u) * 1000000L);
    nanosleep(&req, NULL);
}

static void posixSleepUs(uint32_t us)
{
    if (us == 0) {
        return;
    }
    usleep((useconds_t)us);
}

PlatformSleepInterface NewPlatformSleep(void)
{
    PlatformSleepInterface sleep;
    sleep.sleepMs = posixSleepMs;
    sleep.sleepUs = posixSleepUs;
    return sleep;
}

#elif defined(PLATFORM_FREERTOS)

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if defined(ESP_PLATFORM)
#include <esp_rom_sys.h>
#endif

static void freertosSleepMs(uint32_t ms)
{
    if (ms == 0) {
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static void freertosSleepUs(uint32_t us)
{
    if (us == 0) {
        /* us == 0 时不进入循环，立即返回。 */
        return;
    }

#if defined(ESP_PLATFORM)
    /* ESP-IDF 提供 ROM 级微秒延时。 */
    esp_rom_delay_us(us);
#else
    /* FreeRTOS 无通用微秒级睡眠原语：只能忙等（不释放 CPU）。
     * 计数器只在本忙等分支使用，故就近声明（ESP-IDF 分支不再产生未使用变量告警）。 */
    volatile uint32_t i;
    for (i = 0; i < us; i++) {
        /* 忙等：空循环体，计数器为 volatile 以防被优化掉。 */
    }
#endif
}

PlatformSleepInterface NewPlatformSleep(void)
{
    PlatformSleepInterface sleep;
    sleep.sleepMs = freertosSleepMs;
    sleep.sleepUs = freertosSleepUs;
    return sleep;
}

#endif /* 平台分支 */
