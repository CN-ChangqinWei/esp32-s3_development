#include "platform_memory.h"

/* =====================================================================
 * platform_memory —— 跨平台「内存申请」能力实现（非对象式）
 *
 * 仅按平台选定一组函数指针，返回给调用方；本模块自身不持有任何堆对象，
 * 也不引入任何静态可变状态（无全局变量），符合纯函数式装配约定。
 * ===================================================================== */

#if !PLATFORM_USE_MEMORY

/* 能力关闭：不生成任何函数。
 * 头文件中的 struct PlatformMemoryInterface typedef 仍然存在，
 * 因此本编译单元非空，无需额外占位。 */

#elif defined(PLATFORM_WINDOWS)

#include <stdlib.h>
#include <malloc.h>

static void* windowsMalloc(size_t size)
{
    return malloc(size);
}

static void* windowsCalloc(size_t count, size_t size)
{
    return calloc(count, size);
}

static void* windowsRealloc(void* ptr, size_t size)
{
    return realloc(ptr, size);
}

static void windowsFree(void* ptr)
{
    free(ptr);
}

static void* windowsAlignedMalloc(size_t size, size_t alignment)
{
    /* _aligned_malloc 要求 alignment 为 2 的幂且是 sizeof(void*) 的整数倍；
     * 不满足时其自身返回 NULL，这里直接透传。 */
    return _aligned_malloc(size, alignment);
}

static void windowsAlignedFree(void* ptr)
{
    _aligned_free(ptr);
}

PlatformMemoryInterface NewPlatformMemory(void)
{
    PlatformMemoryInterface mem;
    mem.malloc        = windowsMalloc;
    mem.calloc        = windowsCalloc;
    mem.realloc       = windowsRealloc;
    mem.free          = windowsFree;
    mem.alignedMalloc = windowsAlignedMalloc;
    mem.alignedFree   = windowsAlignedFree;
    return mem;
}

#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)

#include <stdlib.h>

static void* posixMalloc(size_t size)
{
    return malloc(size);
}

static void* posixCalloc(size_t count, size_t size)
{
    return calloc(count, size);
}

static void* posixRealloc(void* ptr, size_t size)
{
    return realloc(ptr, size);
}

static void posixFree(void* ptr)
{
    free(ptr);
}

/* 判断 value 是否为 2 的幂（value 必须是大于 0）。 */
static int isPowerOfTwo(size_t value)
{
    return (value != 0) && ((value & (value - 1)) == 0);
}

static void* posixAlignedMalloc(size_t size, size_t alignment)
{
    void* ptr = NULL;
    /* posix_memalign 要求 alignment 是 sizeof(void*) 的整数倍，且为 2 的幂。
     * 不满足时返回 NULL；对齐申请失败时同样返回 NULL。 */
    if (!isPowerOfTwo(alignment) || (alignment % sizeof(void*)) != 0) {
        return NULL;
    }
    if (posix_memalign(&ptr, alignment, size) != 0) {
        return NULL;
    }
    return ptr;
}

static void posixAlignedFree(void* ptr)
{
    free(ptr);
}

PlatformMemoryInterface NewPlatformMemory(void)
{
    PlatformMemoryInterface mem;
    mem.malloc        = posixMalloc;
    mem.calloc        = posixCalloc;
    mem.realloc       = posixRealloc;
    mem.free          = posixFree;
    mem.alignedMalloc = posixAlignedMalloc;
    mem.alignedFree   = posixAlignedFree;
    return mem;
}

#elif defined(PLATFORM_FREERTOS)

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos_extra.h"
#include <string.h>

#if defined(ESP_PLATFORM)
#include <esp_heap_caps.h>
#endif

static void* freertosMalloc(size_t size)
{
    return pvPortMalloc(size);
}

static void* freertosCalloc(size_t count, size_t size)
{
    size_t total = count * size;
    void* ptr = pvPortMalloc(total);
    if (ptr != NULL) {
        memset(ptr, 0, total);
    }
    return ptr;
}

static void* freertosRealloc(void* ptr, size_t size)
{
    /* 复用 freertos_extra 提供的 pvPortRealloc 实现。 */
    return pvPortRealloc(ptr, size);
}

static void freertosFree(void* ptr)
{
    vPortFree(ptr);
}

#if defined(ESP_PLATFORM)

static void* freertosAlignedMalloc(size_t size, size_t alignment)
{
    return heap_caps_aligned_alloc(alignment, size, MALLOC_CAP_8BIT);
}

static void freertosAlignedFree(void* ptr)
{
    heap_caps_free(ptr);
}

#else

/* 无 heap_caps 支持：退回 pvPortMalloc / vPortFree，
 * 此处仅保证 portBYTE_ALIGNMENT 对齐，不保证调用方期望的 alignment。 */
static void* freertosAlignedMalloc(size_t size, size_t alignment)
{
    (void)alignment;
    return pvPortMalloc(size);
}

static void freertosAlignedFree(void* ptr)
{
    vPortFree(ptr);
}

#endif /* defined(ESP_PLATFORM) */

PlatformMemoryInterface NewPlatformMemory(void)
{
    PlatformMemoryInterface mem;
    mem.malloc        = freertosMalloc;
    mem.calloc        = freertosCalloc;
    mem.realloc       = freertosRealloc;
    mem.free          = freertosFree;
    mem.alignedMalloc = freertosAlignedMalloc;
    mem.alignedFree   = freertosAlignedFree;
    return mem;
}

#endif
