#include "router.h"

#include <string.h>
static Router router={0};
static char* routerMsg=NULL;
char* generateMsg(const char* msg){
    int len=strlen(msg);
    if(len<=0) return NULL;
    char* res=pvPortMalloc(len+1);
    memset(res,0,len+1);
    strcpy(res,msg);
    return res;
}
uint8_t RouterInit(){
    memset(&router, 0, sizeof(Router));
    return 0;
}//初始化全局router单例
uint8_t RouterExec(){
    if(router.taskHeadCur!=router.taskTailCur){
        Task tk=router.taskQue[router.taskHeadCur];
        
        if(tk.handler.handler!=NULL)tk.handler.handler(tk.handler.instance,tk.arg);
        router.taskHeadCur++;
        router.taskHeadCur%=_ROUTER_MAX_TASK_CNT;
        vPortFree(tk.arg);
    }
    
    return 0;
}
uint8_t RouterRegister(uint32_t protocol,RouterHandlerPkg handler){
    if(protocol>=_ROUTER_MAX_CNT) return 1;
    router.handlers[protocol]=handler;
    return 0;
}//注册handlerS

uint8_t RouterAddTask(Task tk){
    uint32_t check = (router.taskHeadCur+1)%(_ROUTER_MAX_TASK_CNT+1);
    if(check==router.taskTailCur) return 1;
    router.taskQue[router.taskTailCur]=tk;
    router.taskTailCur++;
    router.taskTailCur%=_ROUTER_MAX_TASK_CNT+1;
    return 0;
}
void  RouterAnlyPackage(void*package,int len){
    int protocol = *((int*)package);
    
    if(protocol>=_ROUTER_MAX_CNT||protocol<0) {
        vPortFree(package);
        Task tk={router.errHandler,(void*)generateMsg("protocol <0 or > _ROUTER_MAX_CNT")};
        RouterAddTask(tk);
        return;
    }
    RouterHandlerPkg handler = router.handlers[protocol];
    if(NULL==handler.handler){
        vPortFree(package);
        Task tk={router.errHandler,(void*)generateMsg("NULL handler ,no such protocol")};
        RouterAddTask(tk);
        return;
    }
    Task tk ={handler,package};
    RouterAddTask(tk);
}
void    RouterSetErrHandler(RouterHandlerPkg pkg){
    router.errHandler=pkg;
}


