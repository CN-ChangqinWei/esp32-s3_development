#ifndef _PLATFORM_THREAD_H
#define _PLATFORM_THREAD_H

/* 本头会被 C++ 编译单元（Qt 具象层 / demo）包含，故自带 extern "C" 包裹，
   且把本项目 C 头（platform.h 与 thread_object.h）的包含置于包裹**之内**：
   这样无论本头与平台聚合头谁先被包含，platform.h 及其内部的全部 platform_*.h
   都以 C 链接展开，消除「谁先包含」对语言链接的隐式耦合。
   platform.h 与 thread_object.h 自身亦带包裹，此处出现的「extern "C" 嵌套」
   在 C++ 中是合法且幂等的。 */
#ifdef __cplusplus
extern "C" {
#endif

#include "platform.h"
#include "thread_object.h"

/* =====================================================================
 * platform_thread —— 跨平台「线程对象」能力门面（对象式工厂）
 *
 * 职责：
 *   把平台无关的统一配置 PlatformThreadConfig 逐字段映射为「当前平台」
 *   具象 ThreadObject 的构造配置，再调用对应的具体构造，返回装配好的
 *   抽象 ThreadObject*。调用方只依赖抽象 ThreadObjectInterface。
 *
 * 平台差异（由 platform_thread.c 的互斥分支选择，头文件不感知平台）：
 *   Windows : NewThreadObjectWindows  —— stackSize 为字节，另有 safe 字段（本层置 NULL）
 *   Linux   : NewThreadObjectLinux    —— stackSize 为字节（size_t）
 *   macOS   : NewThreadObjectMacos    —— stackSize 为字节（size_t）
 *   FreeRTOS: NewThreadObjectFreertos —— stackSize 映射为 stackDepth（单位「字」），透传 priority/name
 *
 * 内存：本层不做任何分配/释放，堆对象全部由被调用的具象 NewThreadObjectXxx 负责。
 * 依赖：本头反向 #include "platform.h" 安全（platform.h 末尾聚合包含本头，
 *       include guard 保证只展开一次；且平台宏在聚合包含之前已定义完毕）。
 * ===================================================================== */

/* 跨平台统一的线程创建配置（平台差异字段已显式标注） */
typedef struct PlatformThreadConfig{
    int         stackSize;   /* 栈大小：Windows/POSIX 为字节；FreeRTOS 为 xTaskCreate 的字数；<=0 = 平台默认 */
    int         priority;    /* 线程优先级：仅 FreeRTOS 使用，其余平台忽略 */
    const char* name;        /* 线程名：仅 FreeRTOS 使用，可为 NULL；其余平台忽略 */
    void      (*workFunc)(void* param);  /* 线程体（兜底），可为 NULL */
    void       *workParam;               /* 线程体实参 */
} PlatformThreadConfig;

#if PLATFORM_USE_THREAD_OBJECT
/* 按当前平台返回对应具象装配好的抽象对象：windows->thread_object_windows，
   linux->thread_object_linux，macos->thread_object_macos，freertos->thread_object_freertos。
   config 为空返回 NULL；具体构造失败返回 NULL。 */
ThreadObject* NewPlatformThreadObject(PlatformThreadConfig* config);
#endif /* PLATFORM_USE_THREAD_OBJECT */

#ifdef __cplusplus
}
#endif

#endif /* _PLATFORM_THREAD_H */
