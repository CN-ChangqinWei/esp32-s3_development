#ifndef _THREAD_OBJECT_H
#define _THREAD_OBJECT_H
//custom include begin
#include<malloc.h>
//custom include end
#include<stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 本模块公共头统一带 extern "C" 包裹：本头会被 C++ 编译单元（Qt 具象层 / demo）包含，
 * 无包裹会导致 C++ 名字修饰与 C 实现不匹配（链接期 undefined reference）。
 * 标准库头留在包裹之外；本项目的 C 头（断言 / 日志桥接）置于包裹之内 ——
 * extern "C" 可嵌套，被包含头若也自带包裹，在 C++ 下合法且幂等。 */
#include"assert_marco.h"

extern void* (*threadObjectMalloc)(size_t size);
extern void  (*threadObjectFree)(void* p);
typedef struct ThreadObjectInterface{
    int (*start)(void *instance,void (*worker)(void* param),void* param);
    int (*stop)(void *instance);
    int (*getId)(void *instance);
    int (*isRun)(void *instance);
    void (*deleteInstance)(void *instance);
}ThreadObjectInterface;

typedef struct ThreadObject{
    void* instance;
    ThreadObjectInterface interfaces;
}ThreadObject;

ThreadObject* NewThreadObject(void* instance,ThreadObjectInterface interfaces);
int     InitThreadObject(ThreadObject* threadObj,void* instance,ThreadObjectInterface interfaces);
void    DeleteThreadObject(ThreadObject* threadObj);



#ifdef __cplusplus
}
#endif

#endif
