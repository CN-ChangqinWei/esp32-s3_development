#ifndef _TIMER_EXECUTOR_H
#define _TIMER_EXECUTOR_H
#include <stdint.h>
#include <stdlib.h>      /* malloc / free / NULL（治本 2：<malloc.h> 非 ISO C 标准头，改用 <stdlib.h>） */
#include <stddef.h>      /* size_t（模块分配器签名用） */

#ifdef __cplusplus
extern "C" {
#endif

/* 本模块公共头自带 extern "C" 包裹：本头会被 C++ 编译单元（Qt demo）包含，
 * 无包裹会导致 C++ 名字修饰与 timer_executor.c 的 C 定义不匹配（链接期 undefined reference）。
 * 标准库头留在包裹之外；本项目的 C 头（timestamp / thread_object / assert_marco）
 * 一并放进包裹之内 —— extern "C" 可嵌套，被包含头若也自带包裹，在 C++ 下合法且幂等。 */
#include"timestamp.h"
#include"thread_object.h"
#include"assert_marco.h"

//模块级内存接口
extern void* (*timerExecutorMalloc)(size_t size);
extern void  (*timerExecutorFree)(void* p);

typedef struct TimerExecutorConfig {
    void (*event)(void*);
    void* param;
    ThreadObject* threadObj;
    uint64_t (*getTimestamp)();
    void (*sleep)(uint64_t);  
}TimerExecutorConfig;

typedef struct TimerExecutor{
    
    TimerExecutorConfig config;
    uint64_t _lastTime;
    char _isRun;
    char _suicide; // 自杀值
    char _isPause;
    uint64_t _interval;
    uint64_t _sleepTime;
    uint64_t _timeCnt;

    int (*start)(struct TimerExecutor*,uint64_t);
    int (*pause)(struct TimerExecutor*);
    int (*stop)(struct TimerExecutor*);
    int (*isRun)(struct TimerExecutor*);
    int (*conti)(struct TimerExecutor*);
    
    
}TimerExecutor;

TimerExecutor* NewTimerExecutor(TimerExecutorConfig* config);
void DeleteTimerExecutor(TimerExecutor* ins);
int  InitTimerExecutor(TimerExecutor* ins,TimerExecutorConfig* config);

#ifdef __cplusplus
}
#endif

#endif
