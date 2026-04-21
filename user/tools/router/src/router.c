#include "router.h"
#include "freertos/FreeRTOS.h"
#include <string.h>

static void* (*routerMalloc)(size_t size) = pvPortMalloc;
static void  (*routerFree)(void* p) = vPortFree;

static char* generateMsg(const char* msg){
    int len=strlen(msg);
    if(len<=0) return NULL;
    char* res=routerMalloc(len+1);
    memset(res,0,len+1);
    strcpy(res,msg);
    return res;
}

Router* NewRouter(TaskQue* que){
    Router* router = routerMalloc(sizeof(Router));
    if(router != NULL){
        memset(router, 0, sizeof(Router));
        router->taskQue = que;
    }
    return router;
}

void DeleteRouter(Router* router){
    if(router != NULL){
        // 只释放Router本身，不释放外部注入的TaskQue
        routerFree(router);
    }
}

uint8_t RouterInit(Router* router, TaskQue* que){
    if(router == NULL) return 1;
    memset(router, 0, sizeof(Router));
    router->taskQue = que;
    return 0;
}

void RouterExec(Router* router, Task* tk){
    if(router == NULL || tk == NULL) return;
    
    if(tk->handler.handler != NULL){
        tk->handler.handler(tk->handler.instance, tk->arg);
    }
    // 注意：arg 内存由 TaskQue 在任务执行后统一释放
}

uint8_t RouterRegister(Router* router, uint32_t protocol, RouterHandlerPkg handler){
    if(router == NULL) return 1;
    if(protocol >= _ROUTER_MAX_CNT) return 1;
    router->handlers[protocol] = handler;
    return 0;
}

uint8_t RouterAddTask(Router* router, Task tk){
    if(router == NULL) return 1;
    if(router->taskQue == NULL) return 1;
    
    // 构造TaskPackage（适配TaskQue的接口）
    TaskPackage pkg;
    // RouterHandler 和 TaskFunc 签名相同，直接赋值
    pkg.func = tk.handler.handler;
    pkg.instance = tk.handler.instance;  // 传递 instance（如 service）
    pkg.data = tk.arg;
    
    TaskQueAdd(router->taskQue, pkg);
    return 0;
}

void RouterAnlyPackage(Router* router, void* package, int len){
    (void)len;  // 未使用参数
    
    if(router == NULL) {
        routerFree(package);
        return;
    }
    
    int protocol = *((int*)package);
    
    if(protocol >= _ROUTER_MAX_CNT || protocol < 0) {
        routerFree(package);
        if(router->errHandler.handler != NULL){
            Task tk = {{router->errHandler.handler, router->errHandler.instance}, (void*)generateMsg("protocol <0 or > _ROUTER_MAX_CNT")};
            RouterAddTask(router, tk);
        }
        return;
    }
    
    RouterHandlerPkg handler = router->handlers[protocol];
    if(handler.handler == NULL){
        routerFree(package);
        if(router->errHandler.handler != NULL){
            Task tk = {{router->errHandler.handler, router->errHandler.instance}, (void*)generateMsg("NULL handler, no such protocol")};
            RouterAddTask(router, tk);
        }
        return;
    }
    
    Task tk = {handler, package};
    RouterAddTask(router, tk);
}

void RouterSetErrHandler(Router* router, RouterHandlerPkg pkg){
    if(router == NULL) return;
    router->errHandler = pkg;
}
