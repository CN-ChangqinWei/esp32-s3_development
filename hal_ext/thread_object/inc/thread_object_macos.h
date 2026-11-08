#ifndef _THREAD_OBJECT_MACOS_H
#define _THREAD_OBJECT_MACOS_H

#include "thread_object.h"
#include <stddef.h>          /* size_t：不依赖其它头间接引入 */
#include <pthread.h>         /* POSIX 线程（macOS/BSD 实现骨架） */

#ifdef __cplusplus
extern "C" {
#endif

/* ThreadObject 在 macOS(POSIX pthread) 下的配置：由调用方在创建前填充。
 * 该配置会整体拷贝落地到 ThreadObjectMacos::config。 */
typedef struct ThreadObjectMacosConfig{
    size_t stackSize;                 /* 线程栈字节数，0 = 平台默认 */
    void  (*workFunc)(void* param);   /* 线程体（兜底），可为 NULL */
    void  *workParam;                 /* 线程体实参 */
} ThreadObjectMacosConfig;

/* ThreadObject 在 macOS(POSIX pthread) 下的具体实例（具象）。
 * hThread : pthread_create 产出，取消/回收的唯一凭据
 * id      : 线程标识，由 (int)(intptr_t)hThread 显式转换得到（macOS 下 pthread_t 为指针）
 * isRun   : 0 = 未运行/已退出，1 = 运行中（仅 0/1 字面量，禁用 TRUE/FALSE）
 * isContinue : 预留的继续运行标志，本具象暂不使用
 * config  : 配置副本（start 注入 worker 时会被覆盖）
 * interfaces : 本具象实现的接口表副本（抽象包装器持有同一份） */
typedef struct ThreadObjectMacos{
    pthread_t hThread;
    int       id;
    char      isRun;
    char      isContinue;
    ThreadObjectMacosConfig config;
    ThreadObjectInterface   interfaces;
} ThreadObjectMacos;

/* 具体构造：分配抽象对象 ThreadObject 并装配 ThreadObjectMacos 具象实例。
 * config 为空 -> NULL；config 非法 -> NULL。 */
ThreadObject* NewThreadObjectMacos(ThreadObjectMacosConfig* config);

/* 具体构造的内部装配：分配 ThreadObjectMacos 实例并调用抽象构造 InitThreadObject
 * 完成装配（obj 由调用方分配，通常由 NewThreadObjectMacos 传入）。 */
int           InitThreadObjectMacos(ThreadObject* obj);

#ifdef __cplusplus
}
#endif

#endif
