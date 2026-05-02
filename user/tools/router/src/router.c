#include "router.h"
#include "freertos/FreeRTOS.h"
#include <string.h>
#include "esp_log.h"
#define _ROUTER_DEBUG ESP_LOGI
static const char* TAG = "ROUTER";
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
    _ROUTER_DEBUG(TAG, "NewRouter start");
    Router* router = routerMalloc(sizeof(Router));
    if(router != NULL){
        memset(router, 0, sizeof(Router));
        router->taskQue = que;
        _ROUTER_DEBUG(TAG, "NewRouter success");
    } else {
        _ROUTER_DEBUG(TAG, "NewRouter failed: malloc failed");
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
    if(router == NULL || tk == NULL) {
        _ROUTER_DEBUG(TAG, "RouterExec failed: router or tk is NULL");
        return;
    }
    
    _ROUTER_DEBUG(TAG, "RouterExec: executing handler");
    if(tk->handler.handler != NULL){
        tk->handler.handler(tk->handler.instance, tk->arg);
    } else {
        _ROUTER_DEBUG(TAG, "RouterExec: handler is NULL");
    }
    // 注意：arg 内存由 TaskQue 在任务执行后统一释放
}

uint8_t RouterRegister(Router* router, uint32_t protocol, RouterHandlerPkg handler){
    if(router == NULL) {
        _ROUTER_DEBUG(TAG, "RouterRegister failed: router is NULL");
        return 1;
    }
    if(protocol >= _ROUTER_MAX_CNT) {
        _ROUTER_DEBUG(TAG, "RouterRegister failed: protocol %lu out of range", protocol);
        return 1;
    }
    router->handlers[protocol] = handler;
    _ROUTER_DEBUG(TAG, "RouterRegister: protocol %lu registered", protocol);
    return 0;
}

uint8_t RouterAddTask(Router* router, Task tk){
    if(router == NULL) {
        _ROUTER_DEBUG(TAG, "RouterAddTask failed: router is NULL");
        return 1;
    }
    if(router->taskQue == NULL) {
        _ROUTER_DEBUG(TAG, "RouterAddTask failed: taskQue is NULL");
        return 1;
    }

    // 构造TaskPackage（适配TaskQue的接口）
    TaskPackage pkg;
    // RouterHandler 和 TaskFunc 签名相同，直接赋值
    pkg.func = tk.handler.handler;
    pkg.instance = tk.handler.instance;  // 传递 instance（如 service）
    pkg.data = tk.arg;

    uint8_t res = TaskQueAdd(router->taskQue, pkg);
    if(res == 0) {
        _ROUTER_DEBUG(TAG, "RouterAddTask: task added to queue");
    } else {
        _ROUTER_DEBUG(TAG, "RouterAddTask failed: TaskQueAdd returned %d", res);
    }
    return res;
}

void RouterAnlyPackage(Router* router, void* package, int len){
    (void)len;  // 未使用参数

    _ROUTER_DEBUG(TAG, "RouterAnlyPackage: new package received");

    if(router == NULL) {
        _ROUTER_DEBUG(TAG, "RouterAnlyPackage failed: router is NULL, freeing package");
        routerFree(package);
        return;
    }

    int protocol = *((int*)package);
    _ROUTER_DEBUG(TAG, "RouterAnlyPackage: protocol=%d", protocol);

    if(protocol >= _ROUTER_MAX_CNT || protocol < 0) {
        _ROUTER_DEBUG(TAG, "RouterAnlyPackage failed: protocol %d out of range [0, %d)", protocol, _ROUTER_MAX_CNT);
        routerFree(package);
        if(router->errHandler.handler != NULL){
            _ROUTER_DEBUG(TAG, "RouterAnlyPackage: calling errHandler for invalid protocol");
            Task tk = {{router->errHandler.handler, router->errHandler.instance}, (void*)generateMsg("protocol <0 or > _ROUTER_MAX_CNT")};
            RouterAddTask(router, tk);
        }
        return;
    }

    RouterHandlerPkg handler = router->handlers[protocol];
    if(handler.handler == NULL){
        _ROUTER_DEBUG(TAG, "RouterAnlyPackage failed: no handler for protocol %d", protocol);
        routerFree(package);
        if(router->errHandler.handler != NULL){
            _ROUTER_DEBUG(TAG, "RouterAnlyPackage: calling errHandler for NULL handler");
            Task tk = {{router->errHandler.handler, router->errHandler.instance}, (void*)generateMsg("NULL handler, no such protocol")};
            RouterAddTask(router, tk);
        }
        return;
    }

    _ROUTER_DEBUG(TAG, "RouterAnlyPackage: routing protocol %d to handler", protocol);
    Task tk = {handler, package};
    RouterAddTask(router, tk);
}

void RouterSetErrHandler(Router* router, RouterHandlerPkg pkg){
    if(router == NULL) {
        _ROUTER_DEBUG(TAG, "RouterSetErrHandler failed: router is NULL");
        return;
    }
    router->errHandler = pkg;
    _ROUTER_DEBUG(TAG, "RouterSetErrHandler: error handler set");
}
