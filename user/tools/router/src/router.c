#include "router.h"

#include <string.h>

static char* routerMsg=NULL;
static void RouterCommHanlder(void* p){
    if(NULL==p) return;
    Router* r=(Router*)p;
    while(1){
        RouterExec(r);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
void RouterStart(Router* router){
    if(router != NULL){
        xTaskCreate(RouterCommHanlder, "RouterTask", 2048, router, 10, NULL);
    }
}


char* generateMsg(const char* msg){
    int len=strlen(msg);
    if(len<=0) return NULL;
    char* res=pvPortMalloc(len+1);
    memset(res,0,len+1);
    strcpy(res,msg);
    return res;
}

Router* NewRouter(){
    Router* router = pvPortMalloc(sizeof(Router));
    if(router != NULL){
        memset(router, 0, sizeof(Router));
        // 创建互斥锁
        router->mutex = xSemaphoreCreateMutex();
        if(router->mutex == NULL){
            vPortFree(router);
            return NULL;
        }
    }
    return router;
}

void DeleteRouter(Router* router){
    if(router != NULL){
        // 删除互斥锁
        if(router->mutex != NULL){
            vSemaphoreDelete(router->mutex);
        }
        vPortFree(router);
    }
}

uint8_t RouterInit(Router* router){
    if(router == NULL) return 1;
    // 保存互斥锁句柄
    SemaphoreHandle_t oldMutex = router->mutex;
    memset(router, 0, sizeof(Router));
    // 如果之前没有锁，创建新锁；否则恢复旧锁
    if(oldMutex == NULL){
        router->mutex = xSemaphoreCreateMutex();
        if(router->mutex == NULL) return 1;
    } else {
        router->mutex = oldMutex;
    }
    return 0;
}//初始化Router实例

uint8_t RouterExec(Router* router){
    if(router == NULL) return 1;
    if(router->mutex == NULL) return 1;
    
    // 获取互斥锁
    if(xSemaphoreTake(router->mutex, portMAX_DELAY) != pdTRUE){
        return 1;
    }
    
    if(router->taskHeadCur!=router->taskTailCur){
        Task tk=router->taskQue[router->taskHeadCur];
        
        router->taskHeadCur++;
        router->taskHeadCur%=_ROUTER_MAX_TASK_CNT;
        
        // 释放锁后再执行handler（避免死锁）
        xSemaphoreGive(router->mutex);
        
        if(tk.handler.handler!=NULL){
            tk.handler.handler(tk.handler.instance,tk.arg);
        }
        vPortFree(tk.arg);
        return 0;
    }
    
    // 释放互斥锁
    xSemaphoreGive(router->mutex);
    return 0;
}

uint8_t RouterRegister(Router* router,uint32_t protocol,RouterHandlerPkg handler){
    if(router == NULL) return 1;
    if(protocol>=_ROUTER_MAX_CNT) return 1;
    router->handlers[protocol]=handler;
    return 0;
}//注册handlerS

uint8_t RouterAddTask(Router* router,Task tk){
    if(router == NULL) return 1;
    if(router->mutex == NULL) return 1;
    
    // 获取互斥锁
    if(xSemaphoreTake(router->mutex, portMAX_DELAY) != pdTRUE){
        return 1;
    }
    
    uint32_t check = (router->taskHeadCur+1)%(_ROUTER_MAX_TASK_CNT+1);
    if(check==router->taskTailCur) {
        xSemaphoreGive(router->mutex);//释放锁
        return 1;
    }
    router->taskQue[router->taskTailCur]=tk;
    router->taskTailCur++;
    router->taskTailCur%=_ROUTER_MAX_TASK_CNT+1;
    
    // 释放互斥锁
    xSemaphoreGive(router->mutex);
    return 0;
}

void  RouterAnlyPackage(Router* router,void*package,int len){
    if(router == NULL) {
        vPortFree(package);
        return;
    }
    int protocol = *((int*)package);
    
    if(protocol>=_ROUTER_MAX_CNT||protocol<0) {
        vPortFree(package);
        Task tk={router->errHandler,(void*)generateMsg("protocol <0 or > _ROUTER_MAX_CNT")};
        RouterAddTask(router,tk);
        return;
    }
    RouterHandlerPkg handler = router->handlers[protocol];
    if(NULL==handler.handler){
        vPortFree(package);
        Task tk={router->errHandler,(void*)generateMsg("NULL handler ,no such protocol")};
        RouterAddTask(router,tk);
        return;
    }
    Task tk ={handler,package};
    RouterAddTask(router,tk);
}

void    RouterSetErrHandler(Router* router,RouterHandlerPkg pkg){
    if(router == NULL) return;
    router->errHandler=pkg;
}
