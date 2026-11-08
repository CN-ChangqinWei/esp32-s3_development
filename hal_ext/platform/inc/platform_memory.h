#ifndef _PLATFORM_MEMORY_H
#define _PLATFORM_MEMORY_H

#include <stddef.h>

/* 本头会被 C++ 编译单元（Qt 具象层、demo）包含，故自带 extern "C" 包裹，
   保证 C++ 单元看到的是 C 名字修饰（避免链接期 undefined reference）。
   标准库头（<stddef.h>）留在包裹之外；本项目的 C 头（platform.h）与
   下方全部 typedef / 函数声明一并放进包裹之内。platform.h 自身亦带包裹，
   此处出现的「extern "C" 嵌套」在 C++ 中是合法的。 */
#ifdef __cplusplus
extern "C" {
#endif

#include "platform.h"

/* 跨平台内存申请接口（非对象式：一组按平台选定的函数指针）。
 * 平台差异：
 *   Windows            : malloc/calloc/realloc/free + _aligned_malloc/_aligned_free
 *   Linux / macOS      : malloc/calloc/realloc/free + posix_memalign/free
 *   FreeRTOS(ESP-IDF)  : pvPortMalloc / vPortFree（realloc 复用 freertos_extra 的 pvPortRealloc），
 *                        calloc = pvPortMalloc + memset
 */
typedef struct PlatformMemoryInterface{
    void* (*malloc)(size_t size);
    void* (*calloc)(size_t count, size_t size);
    void* (*realloc)(void* ptr, size_t size);
    void  (*free)(void* ptr);
    void* (*alignedMalloc)(size_t size, size_t alignment);
    void  (*alignedFree)(void* ptr);
} PlatformMemoryInterface;

/* 返回当前平台的内存申请函数集；能力关闭(PLATFORM_USE_MEMORY==0)时不生成此函数 */
#if PLATFORM_USE_MEMORY
PlatformMemoryInterface NewPlatformMemory(void);
#endif /* PLATFORM_USE_MEMORY */

#ifdef __cplusplus
}
#endif

#endif /* _PLATFORM_MEMORY_H */
