#include "platform_sync.h"

#include <stdlib.h>
#include <string.h>

/* =====================================================================
 * platform_sync —— 跨平台「同步 / 互斥量」能力实现（对象式）
 *
 * 组织方式：
 *   - 模块级分配器 platformSyncMalloc / platformSyncFree 无条件定义
 *     （默认指向标准库 malloc/free，可整体替换）；
 *   - 抽象构造 / 析构（NewPlatformMutex / DeletePlatformMutex）平台无关，
 *     仅在 PLATFORM_USE_SYNC 开启时定义，避免各平台分支重复；
 *   - 各平台具象结构体与接口函数以「能力开关 + 平台宏」组织为互斥分支，
 *     同一编译单元只包含当前平台的平台头（严禁同时包含多个平台头）。
 *
 * 内存：本模块一切堆分配/释放均走 platformSyncMalloc / platformSyncFree，
 *       严禁裸调 malloc/free。
 * 健壮性：所有接口函数先判空/判初始化状态再执行业务。
 * ===================================================================== */

/* ===== 模块级内存管理接口（默认使用标准库 malloc/free，可整体替换） ===== */
void* (*platformSyncMalloc)(size_t size) = malloc;
void  (*platformSyncFree)(void* p)       = free;

#if PLATFORM_USE_SYNC

/* ===== 抽象类构造函数（平台无关） =====
 * 参数表固定：只做「分配抽象对象 + 存 instance + 存 interfaces」，
 * 不在此处分配具象实例。 */
PlatformMutex* NewPlatformMutex(void* instance, PlatformMutexInterface interfaces)
{
    PlatformMutex* res;

    if (NULL == instance) return NULL;

    res = (PlatformMutex*)platformSyncMalloc(sizeof(PlatformMutex));
    if (NULL == res) return NULL;

    res->instance   = instance;
    res->interfaces = interfaces;
    return res;
}

/* ===== 抽象类析构函数（平台无关） =====
 * ① 先调用具象实例的析构函数销毁具象；② 释放抽象对象；③ 入参指针置 NULL。 */
void DeletePlatformMutex(PlatformMutex** pMutex)
{
    if (NULL == pMutex || NULL == *pMutex) return;

    /* ① 先销毁具体实例（含平台原语） */
    if (NULL != (*pMutex)->interfaces.deleteInstance) {
        (*pMutex)->interfaces.deleteInstance((*pMutex)->instance);
    }

    /* ② 释放抽象对象本身 */
    platformSyncFree(*pMutex);

    /* ③ 防止悬空指针 */
    *pMutex = NULL;
}

#endif /* PLATFORM_USE_SYNC */

/* =====================================================================
 * 平台具象分支（互斥）
 * ===================================================================== */
#if !PLATFORM_USE_SYNC

/* 能力关闭：不生成任何函数。
 * 头文件中的 struct PlatformMutexInterface / PlatformMutex typedef 仍然存在，
 * 因此本编译单元非空，无需额外占位。 */

#elif defined(PLATFORM_WINDOWS)

#include <windows.h>

/* Windows 具象：互斥量句柄 */
typedef struct PlatformMutexWindows{
    HANDLE hMutex;
} PlatformMutexWindows;

static int windowsLock(void* instance)
{
    PlatformMutexWindows* self = (PlatformMutexWindows*)instance;
    if (NULL == self) return -1;
    if (NULL == self->hMutex) return -1;
    if (WAIT_OBJECT_0 != WaitForSingleObject(self->hMutex, INFINITE)) {
        return -1;
    }
    return 0;
}

static int windowsTryLock(void* instance, uint32_t timeoutMs)
{
    DWORD waitMs;
    DWORD waitResult;
    PlatformMutexWindows* self = (PlatformMutexWindows*)instance;

    if (NULL == self) return -1;
    if (NULL == self->hMutex) return -1;

    /* timeoutMs == UINT32_MAX 约定为「无限等待」 */
    waitMs = (UINT32_MAX == timeoutMs) ? INFINITE : (DWORD)timeoutMs;

    waitResult = WaitForSingleObject(self->hMutex, waitMs);
    return (WAIT_OBJECT_0 == waitResult) ? 0 : -1;
}

static int windowsUnlock(void* instance)
{
    PlatformMutexWindows* self = (PlatformMutexWindows*)instance;
    if (NULL == self) return -1;
    if (NULL == self->hMutex) return -1;
    return ReleaseMutex(self->hMutex) ? 0 : -1;
}

static void windowsDeleteInstance(void* instance)
{
    PlatformMutexWindows* self = (PlatformMutexWindows*)instance;
    if (NULL == self) return;
    if (NULL != self->hMutex) {
        CloseHandle(self->hMutex);
        self->hMutex = NULL;
    }
    platformSyncFree(self);
}

/* 集中返回虚表，避免手写漏项 */
#define _RETURN_PLATFORM_MUTEX_INTERFACE_WINDOWS \
PlatformMutexInterface interfaces = {            \
        .lock           = windowsLock,           \
        .tryLock        = windowsTryLock,        \
        .unlock         = windowsUnlock,         \
        .deleteInstance = windowsDeleteInstance  \
    };                                           \
return interfaces;

static PlatformMutexInterface PlatformMutexWindowsInterface(void)
{
    _RETURN_PLATFORM_MUTEX_INTERFACE_WINDOWS
}

PlatformMutex* NewPlatformMutexForPlatform(void)
{
    PlatformMutexWindows* ins;
    PlatformMutex* abstractObj;

    /* ① 分配具象实例并清零 */
    ins = (PlatformMutexWindows*)platformSyncMalloc(sizeof(PlatformMutexWindows));
    if (NULL == ins) return NULL;
    memset(ins, 0, sizeof(PlatformMutexWindows));

    /* ② 初始化平台原语，失败则释放具象并返回 NULL */
    ins->hMutex = CreateMutex(NULL, FALSE, NULL);
    if (NULL == ins->hMutex) {
        platformSyncFree(ins);
        return NULL;
    }

    /* ③ 必须经由抽象构造完成装配（严禁手工赋值 instance / interfaces） */
    abstractObj = NewPlatformMutex(ins, PlatformMutexWindowsInterface());
    if (NULL == abstractObj) {
        /* 抽象构造失败：销毁已分配的具象实例（含平台原语），避免泄漏 */
        windowsDeleteInstance(ins);
        return NULL;
    }
    return abstractObj;
}

#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)

#include <pthread.h>
#include <time.h>
#include <sched.h>   /* sched_yield：轮询回退分支使用 */
#include <unistd.h>  /* _POSIX_TIMEOUTS 特性宏 */

/* POSIX 具象：互斥量 + 初始化标志（防止误用未初始化的互斥量） */
typedef struct PlatformMutexPosix{
    pthread_mutex_t mtx;
    int inited;
} PlatformMutexPosix;

static int posixLock(void* instance)
{
    PlatformMutexPosix* self = (PlatformMutexPosix*)instance;
    if (NULL == self) return -1;
    if (0 == self->inited) return -1;
    return (0 == pthread_mutex_lock(&self->mtx)) ? 0 : -1;
}

static int posixTryLock(void* instance, uint32_t timeoutMs)
{
    PlatformMutexPosix* self = (PlatformMutexPosix*)instance;

    if (NULL == self) return -1;
    if (0 == self->inited) return -1;

    /* timeoutMs == 0：仅尝试一次 */
    if (0 == timeoutMs) {
        return (0 == pthread_mutex_trylock(&self->mtx)) ? 0 : -1;
    }

    /* timeoutMs == UINT32_MAX：无限等待，等价于 lock */
    if (UINT32_MAX == timeoutMs) {
        return (0 == pthread_mutex_lock(&self->mtx)) ? 0 : -1;
    }

#if defined(_POSIX_TIMEOUTS) && (_POSIX_TIMEOUTS > 0)
    /* 平台提供 pthread_mutex_timedlock：使用内核级超时等待。 */
    {
        struct timespec ts;
        if (0 != clock_gettime(CLOCK_REALTIME, &ts)) {
            return -1;
        }
        ts.tv_sec  += (time_t)(timeoutMs / 1000U);
        ts.tv_nsec += (long)((timeoutMs % 1000U) * 1000000L);
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec  += 1;
            ts.tv_nsec -= 1000000000L;
        }
        return (0 == pthread_mutex_timedlock(&self->mtx, &ts)) ? 0 : -1;
    }
#else
    /* 该平台未提供 pthread_mutex_timedlock（例如 macOS：_POSIX_TIMEOUTS 为 -1），
     * 降级为「trylock 轮询」：反复尝试，失败间用 sched_yield 让出 CPU。
     * 说明：轮询不保证精确超时窗口，且 CPU 占用高于内核级 timedlock；
     *       若需要严格/高效超时，请改用平台专用同步原语。 */
    {
        uint32_t waitedMs = 0;
        const uint32_t stepMs = 1U;
        for (;;) {
            if (0 == pthread_mutex_trylock(&self->mtx)) {
                return 0;
            }
            if (waitedMs >= timeoutMs) {
                return -1;
            }
            sched_yield();
            waitedMs += stepMs;
        }
    }
#endif /* _POSIX_TIMEOUTS */
}

static int posixUnlock(void* instance)
{
    PlatformMutexPosix* self = (PlatformMutexPosix*)instance;
    if (NULL == self) return -1;
    if (0 == self->inited) return -1;
    return (0 == pthread_mutex_unlock(&self->mtx)) ? 0 : -1;
}

static void posixDeleteInstance(void* instance)
{
    PlatformMutexPosix* self = (PlatformMutexPosix*)instance;
    if (NULL == self) return;
    if (0 != self->inited) {
        pthread_mutex_destroy(&self->mtx);
        self->inited = 0;
    }
    platformSyncFree(self);
}

/* 集中返回虚表，避免手写漏项 */
#define _RETURN_PLATFORM_MUTEX_INTERFACE_POSIX \
PlatformMutexInterface interfaces = {          \
        .lock           = posixLock,           \
        .tryLock        = posixTryLock,        \
        .unlock         = posixUnlock,         \
        .deleteInstance = posixDeleteInstance  \
    };                                         \
return interfaces;

static PlatformMutexInterface PlatformMutexPosixInterface(void)
{
    _RETURN_PLATFORM_MUTEX_INTERFACE_POSIX
}

PlatformMutex* NewPlatformMutexForPlatform(void)
{
    PlatformMutexPosix* ins;
    PlatformMutex* abstractObj;

    /* ① 分配具象实例并清零 */
    ins = (PlatformMutexPosix*)platformSyncMalloc(sizeof(PlatformMutexPosix));
    if (NULL == ins) return NULL;
    memset(ins, 0, sizeof(PlatformMutexPosix));

    /* ② 初始化平台原语，失败则释放具象并返回 NULL */
    if (0 != pthread_mutex_init(&ins->mtx, NULL)) {
        platformSyncFree(ins);
        return NULL;
    }
    ins->inited = 1;

    /* ③ 必须经由抽象构造完成装配（严禁手工赋值 instance / interfaces） */
    abstractObj = NewPlatformMutex(ins, PlatformMutexPosixInterface());
    if (NULL == abstractObj) {
        /* 抽象构造失败：销毁已分配的具象实例（含平台原语），避免泄漏 */
        posixDeleteInstance(ins);
        return NULL;
    }
    return abstractObj;
}

#elif defined(PLATFORM_FREERTOS)

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/* FreeRTOS 具象：互斥信号量句柄 */
typedef struct PlatformMutexFreertos{
    SemaphoreHandle_t hMutex;
} PlatformMutexFreertos;

static int freertosLock(void* instance)
{
    PlatformMutexFreertos* self = (PlatformMutexFreertos*)instance;
    if (NULL == self) return -1;
    if (NULL == self->hMutex) return -1;
    return (pdTRUE == xSemaphoreTake(self->hMutex, portMAX_DELAY)) ? 0 : -1;
}

static int freertosTryLock(void* instance, uint32_t timeoutMs)
{
    TickType_t ticks;
    PlatformMutexFreertos* self = (PlatformMutexFreertos*)instance;

    if (NULL == self) return -1;
    if (NULL == self->hMutex) return -1;

    /* 语义约定：timeoutMs == UINT32_MAX 表示无限等待；0 表示不等待 */
    if (UINT32_MAX == timeoutMs) {
        ticks = portMAX_DELAY;
    } else if (0 == timeoutMs) {
        ticks = 0;
    } else {
        ticks = pdMS_TO_TICKS(timeoutMs);
    }

    return (pdTRUE == xSemaphoreTake(self->hMutex, ticks)) ? 0 : -1;
}

static int freertosUnlock(void* instance)
{
    PlatformMutexFreertos* self = (PlatformMutexFreertos*)instance;
    if (NULL == self) return -1;
    if (NULL == self->hMutex) return -1;
    return (pdTRUE == xSemaphoreGive(self->hMutex)) ? 0 : -1;
}

static void freertosDeleteInstance(void* instance)
{
    PlatformMutexFreertos* self = (PlatformMutexFreertos*)instance;
    if (NULL == self) return;
    if (NULL != self->hMutex) {
        vSemaphoreDelete(self->hMutex);
        self->hMutex = NULL;
    }
    platformSyncFree(self);
}

/* 集中返回虚表，避免手写漏项 */
#define _RETURN_PLATFORM_MUTEX_INTERFACE_FREERTOS \
PlatformMutexInterface interfaces = {             \
        .lock           = freertosLock,           \
        .tryLock        = freertosTryLock,        \
        .unlock         = freertosUnlock,         \
        .deleteInstance = freertosDeleteInstance  \
    };                                            \
return interfaces;

static PlatformMutexInterface PlatformMutexFreertosInterface(void)
{
    _RETURN_PLATFORM_MUTEX_INTERFACE_FREERTOS
}

PlatformMutex* NewPlatformMutexForPlatform(void)
{
    PlatformMutexFreertos* ins;
    PlatformMutex* abstractObj;

    /* ① 分配具象实例并清零 */
    ins = (PlatformMutexFreertos*)platformSyncMalloc(sizeof(PlatformMutexFreertos));
    if (NULL == ins) return NULL;
    memset(ins, 0, sizeof(PlatformMutexFreertos));

    /* ② 初始化平台原语，失败则释放具象并返回 NULL */
    ins->hMutex = xSemaphoreCreateMutex();
    if (NULL == ins->hMutex) {
        platformSyncFree(ins);
        return NULL;
    }

    /* ③ 必须经由抽象构造完成装配（严禁手工赋值 instance / interfaces） */
    abstractObj = NewPlatformMutex(ins, PlatformMutexFreertosInterface());
    if (NULL == abstractObj) {
        /* 抽象构造失败：销毁已分配的具象实例（含平台原语），避免泄漏 */
        freertosDeleteInstance(ins);
        return NULL;
    }
    return abstractObj;
}

#endif /* 能力开关 / 平台互斥分支 */
