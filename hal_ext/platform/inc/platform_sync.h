#ifndef _PLATFORM_SYNC_H
#define _PLATFORM_SYNC_H

#include <stdint.h>
#include <stddef.h>

/* 本头会被 C++ 编译单元（Qt 具象层 / demo）包含，故自带 extern "C" 包裹，
   且把本项目 C 头 platform.h 的包含置于包裹**之内**：这样无论本头与平台聚合头
   谁先被包含，platform.h 及其内部的全部 platform_*.h 都以 C 链接展开，
   消除「谁先包含」对语言链接的隐式耦合。platform.h 自身亦带包裹，
   此处出现的「extern "C" 嵌套」在 C++ 中是合法且幂等的。
   标准库头（<stdint.h> / <stddef.h>）留在包裹之外，与 platform_memory.h / platform_timestamp.h 一致。 */
#ifdef __cplusplus
extern "C" {
#endif

#include "platform.h"

/* =====================================================================
 * platform_sync —— 跨平台「同步 / 互斥量」能力门面（对象式工厂）
 *
 * 遵循 OOP in C 约定：
 *   - PlatformMutexInterface 为虚表；抽象类结构体 PlatformMutex 只放
 *     instance 与 interfaces 两个字段（字段名统一为 interfaces，禁用旧短名）；
 *   - 抽象构造 NewPlatformMutex(instance, interfaces) 参数表固定，职责仅三件：
 *     分配抽象对象 → instance = ins → interfaces = ifaces；不分配具象实例；
 *   - 具体构造 NewPlatformMutexForPlatform() 按当前平台装配具象（走
 *     platformSyncMalloc），内部调用抽象构造完成装配；
 *   - DeletePlatformMutex 先销毁具象（interfaces.deleteInstance）再释放抽象对象，
 *     并把入参指针置 NULL 防悬空；
 *   - 接口表必含 deleteInstance 槽位（不得命名为 delete）。
 *
 * ---------------------------------------------------------------------
 * 返回约定（消费方务必注意）
 * ---------------------------------------------------------------------
 *   - lock   : 成功返回 0，失败返回负值；
 *   - tryLock: 成功（在 timeoutMs 内取得锁）返回 0，超时/失败返回负值；
 *              timeoutMs == UINT32_MAX 约定为「无限等待」（等价于 lock）；
 *              timeoutMs == 0 表示「不等待，仅尝试一次」；
 *   - unlock : 成功返回 0，失败返回负值。
 *
 * 平台差异（由 platform_sync.c 的互斥分支选择，头文件不感知平台）：
 *   Windows            : CreateMutex/ReleaseMutex（HANDLE）
 *   Linux / macOS      : pthread_mutex_t（POSIX）
 *   FreeRTOS(ESP-IDF)  : xSemaphoreCreateMutex（SemaphoreHandle_t）
 *
 * 内存：本模块堆分配/释放一律走 platformSyncMalloc / platformSyncFree。
 * 依赖：本头反向 #include "platform.h" 安全（platform.h 末尾聚合本头，
 *       include guard 保证只展开一次；平台宏在聚合包含之前已定义完毕）。
 * ===================================================================== */

/* 跨平台互斥量接口（对象式虚表） */
typedef struct PlatformMutexInterface{
    int  (*lock)(void* instance);
    int  (*tryLock)(void* instance, uint32_t timeoutMs);
    int  (*unlock)(void* instance);
    void (*deleteInstance)(void* instance);
} PlatformMutexInterface;

/* 抽象类结构体：只放 instance 与 interfaces 两个字段 */
typedef struct PlatformMutex{
    void*                  instance;
    PlatformMutexInterface interfaces;
} PlatformMutex;

/* 模块级分配器（可整体替换；实现文件里默认指向 malloc/free） */
extern void* (*platformSyncMalloc)(size_t size);
extern void  (*platformSyncFree)(void* p);

#if PLATFORM_USE_SYNC
/* 抽象构造：参数表固定，只做「分配抽象对象 + 存 instance + 存 interfaces」 */
PlatformMutex* NewPlatformMutex(void* instance, PlatformMutexInterface interfaces);
/* 具体构造：按当前平台装配具象，内部必须调用抽象构造完成装配 */
PlatformMutex* NewPlatformMutexForPlatform(void);
void           DeletePlatformMutex(PlatformMutex** pMutex);
#endif /* PLATFORM_USE_SYNC */

#ifdef __cplusplus
}
#endif

#endif /* _PLATFORM_SYNC_H */
