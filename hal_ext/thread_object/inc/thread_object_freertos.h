#ifndef _THREAD_OBJECT_FREERTOS_H
#define _THREAD_OBJECT_FREERTOS_H
#include "thread_object.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 本平台具象头会被 C++ 编译单元包含：统一带 extern "C" 包裹，保证具象声明按 C 链接
 * 规范导出，避免 C++ 名字修饰与 C 实现不匹配（链接期 undefined reference）。
 * include 保持在包裹之外，与 thread_object_linux.h / thread_object_macos.h 写法一致，
 * 同时避免把 FreeRTOS 头拉进 C 链接区。 */
/* ThreadObject 在 FreeRTOS(ESP-IDF 风格)下的构造配置。
 * 由调用方（组合根）填写后交给 NewThreadObjectFreertos。
 * 注意：workFunc/workParam 仅作兜底；start() 注入的 worker/param 会覆盖它们。 */
typedef struct ThreadObjectFreertosConfig{
    int         stackDepth;           /* xTaskCreate 的 usStackDepth（单位：字，不是字节）；<=0 时实现侧取默认值 */
    int         priority;             /* 任务优先级，直接透传 xTaskCreate */
    const char* name;                 /* 任务名，可为 NULL（实现侧给默认名） */
    void      (*workFunc)(void* param);
    void      *workParam;
} ThreadObjectFreertosConfig;

/* FreeRTOS 具象实例：持有任务句柄与运行状态。
 * getId() 语义：返回 (int)(uintptr_t)hThread，即任务句柄降位得到的整型标识；
 *            任务未创建或句柄已被清空时返回 0。
 * isRun() 语义：返回 0/1，非 pdPASS/pdFAIL。 */
typedef struct ThreadObjectFreertos{
    TaskHandle_t hThread;
    int          id;
    char         isRun;
    char         isContinue;
    ThreadObjectFreertosConfig config;
    ThreadObjectInterface      interfaces;
} ThreadObjectFreertos;

ThreadObject* NewThreadObjectFreertos(ThreadObjectFreertosConfig* config);
int           InitThreadObjectFreertos(ThreadObject* obj);

#ifdef __cplusplus
}
#endif
#endif
