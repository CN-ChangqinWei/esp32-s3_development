#ifndef _THREAD_OBJECT_LINUX_H
#define _THREAD_OBJECT_LINUX_H
#include "thread_object.h"
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ThreadObject 在 Linux(POSIX pthread) 下的具象配置。
 * 组合根一般只填 stackSize（0 = 平台默认栈），线程体与实参通常在
 * start() 时注入；workFunc/workParam 仅作兜底使用。 */
typedef struct ThreadObjectLinuxConfig{
    size_t stackSize;                 /* 线程栈字节数，0 = 平台默认 */
    void  (*workFunc)(void* param);   /* 线程体（兜底），可为 NULL */
    void  *workParam;                 /* 线程体实参 */
} ThreadObjectLinuxConfig;

/* ThreadObject 在 Linux 下的具象实例。
 * hThread 为 POSIX 线程句柄（glibc 下即 unsigned long）：
 *   非 0 = 已创建且尚未回收；0 = 尚未创建或已回收。
 * id 为 id 语义说明：pthread_t 是不可移植的整数类型，无法直接当作 int 对外，
 *   故在 start() 创建线程后用 (int)(intptr_t) 显式转换得到日志/标识用 id，
 *   显式转换可避免隐式截断告警；getId() 返回该字段。 */
typedef struct ThreadObjectLinux{
    pthread_t hThread;
    int       id;
    char      isRun;
    char      isContinue;
    ThreadObjectLinuxConfig config;
    ThreadObjectInterface   interfaces;
} ThreadObjectLinux;

/* 具体构造：分配抽象 ThreadObject + 具象实例，并落地 config。失败返回 NULL。 */
ThreadObject* NewThreadObjectLinux(ThreadObjectLinuxConfig* config);
/* 具体初始化：分配并装配具象实例，填充 obj->instance 与 obj->interfaces。成功返回 0，失败返回负错误码。 */
int           InitThreadObjectLinux(ThreadObject* obj);

#ifdef __cplusplus
}
#endif

#endif
