#include "timestamp.h"

#if defined(_WIN32) || defined(_WIN64)
    #define _WIN32_WINNT 0x0600  /* 支持 Vista 及以上 */
    #include <windows.h>

    /* Windows 平台使用 QueryPerformanceCounter */
    static LARGE_INTEGER get_frequency(void) {
        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);
        return freq;
    }

    /* 获取高精度计数器值 */
    static LARGE_INTEGER get_counter(void) {
        LARGE_INTEGER count;
        QueryPerformanceCounter(&count);
        return count;
    }

    uint64_t get_timestamp_ms(void) {
        LARGE_INTEGER freq = get_frequency();
        LARGE_INTEGER count = get_counter();
        /* 毫秒 = 计数器值 * 1000 / 频率 */
        return (uint64_t)((count.QuadPart * 1000) / freq.QuadPart);
    }

    uint64_t get_timestamp_us(void) {
        LARGE_INTEGER freq = get_frequency();
        LARGE_INTEGER count = get_counter();
        return (uint64_t)((count.QuadPart * 1000000) / freq.QuadPart);
    }

    uint64_t get_timestamp_ns(void) {
        LARGE_INTEGER freq = get_frequency();
        LARGE_INTEGER count = get_counter();
        return (uint64_t)((count.QuadPart * 1000000000) / freq.QuadPart);
    }

#else
    /* Linux / macOS / BSD 使用 POSIX clock_gettime */
    #include <time.h>

    /* 获取当前时间（秒 + 纳秒） */
    static struct timespec get_time(void) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        return ts;
    }

    uint64_t get_timestamp_ms(void) {
        struct timespec ts = get_time();
        return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    }

    uint64_t get_timestamp_us(void) {
        struct timespec ts = get_time();
        return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
    }

    uint64_t get_timestamp_ns(void) {
        struct timespec ts = get_time();
        return (uint64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
    }

#endif /* _WIN32 */