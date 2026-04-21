#ifndef _ROUTER_H
#define _ROUTER_H
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "task_que.h"

#define _ROUTER_MAX_CNT 100

typedef uint16_t (*RouterHandler)(void*instance,void*arg);
typedef struct{
    RouterHandler handler;
    void*       instance;
}RouterHandlerPkg;

typedef struct{
    RouterHandlerPkg handler;
    void*       arg;
}Task;

typedef struct{
    RouterHandlerPkg handlers[_ROUTER_MAX_CNT];//存储handler的协议映射表
    RouterHandlerPkg errHandler;
    TaskQue* taskQue;//外部注入的任务队列
}Router;

Router* NewRouter(TaskQue* que);//创建Router实例，注入外部任务队列
void DeleteRouter(Router* router);//删除Router实例（不删除TaskQue）
uint8_t RouterInit(Router* router, TaskQue* que);//初始化Router实例
void    RouterExec(Router* router, Task* tk);//执行单个任务（由TaskQue调用）
uint8_t RouterRegister(Router* router,uint32_t protocol,RouterHandlerPkg handler);//注册handler
uint8_t RouterAddTask(Router* router,Task tk);//添加任务到队列
void    RouterAnlyPackage(Router* router,void* package,int len);//解析数据包并添加任务
void    RouterSetErrHandler(Router* router,RouterHandlerPkg pkg);
#endif
