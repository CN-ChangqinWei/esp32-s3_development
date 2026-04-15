#ifndef _ROUTER_H
#define _ROUTER_H
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#define _ROUTER_MAX_CNT 100
#define _ROUTER_MAX_TASK_CNT 100
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
    uint32_t handlersLen;
    Task     taskQue[_ROUTER_MAX_TASK_CNT+1];//存储任务的循环队列
    uint32_t taskHeadCur;//循环队列头
    uint32_t taskTailCur;//循环队列尾部
    uint8_t  execFlag;
    RouterHandlerPkg errHandler;
    SemaphoreHandle_t mutex;//互斥锁，保护任务队列
}Router;

Router* NewRouter();//创建Router实例
void DeleteRouter(Router* router);//删除Router实例
uint8_t RouterInit(Router* router);//初始化Router实例
uint8_t RouterExec(Router* router);
uint8_t RouterRegister(Router* router,uint32_t protocol,RouterHandlerPkg handler);//注册handler
uint8_t RouterAddTask(Router* router,Task tk);
void    RouterAnlyPackage(Router* router,void*,int len);
void    RouterStopExec(Router* router);
void    RouterSetErrHandler(Router* router,RouterHandlerPkg pkg);
void    RouterStart(Router* router);
#endif
