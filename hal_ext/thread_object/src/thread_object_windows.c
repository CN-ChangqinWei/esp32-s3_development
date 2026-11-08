#include"thread_object_windows.h"

/* 【平台隔离】Win32 系统头只许出现在本 .c（公共头 thread_object_windows.h 不引入任何平台头）：
 *   <windows.h> ⇒ HANDLE / DWORD / TRUE / FALSE、CloseHandle / TerminateThread；
 *   <process.h> ⇒ _beginthreadex；
 *   <string.h>  ⇒ memset（不再靠系统头传递）。 */
#include <windows.h>
#include <process.h>
#include <string.h>

/* 公共头里 hThread / id 以等宽基本类型（void* / unsigned long）承载，
 * 本文件在使用处按 HANDLE / DWORD 语义显式转换，布局与原来完全一致。 */

static int start(void *instance, void (*worker)(void* param), void* param);
static int stop(void *instance);
static int getId(void *instance);
static int isRun(void *instance);
static void deleteInstance(void *instance);
static int isConfigIllegal(ThreadObjectWindowsConfig* config);
static void threadEntry(void* instance);

ThreadObject* NewThreadObjectWindows(ThreadObjectWindowsConfig* config){
    ASSERT_NOT_NULL(config,"config is NULL",NULL);
    ASSERT_NULL(isConfigIllegal(config),"config is illegal",NULL);
    
    ThreadObject* obj = threadObjectMalloc(sizeof(ThreadObject));
    ASSERT_NOT_NULL(obj,"threadObjectMalloc ThreadObject error",NULL);
    memset(obj,0,sizeof(ThreadObject));

    int err = InitThreadObjectWindows(obj);
    if(err){
        threadObjectFree(obj);
        obj=NULL;
        return obj;
    }

    /* 配置落地：把调用方传入的配置保存到具体实例（worker/start 后续依赖它） */
    {
        ThreadObjectWindows* instance = (ThreadObjectWindows*)obj->instance;
        instance->config = *config;
    }

    return obj;
}
int InitThreadObjectWindows(ThreadObject* obj){
    ThreadObjectWindows* instance = threadObjectMalloc(sizeof(ThreadObjectWindows));
    ASSERT_NOT_NULL(instance,"threadObjectMalloc error",-1);
    memset(instance,0,sizeof(ThreadObjectWindows));

    obj->instance=instance;
    ThreadObjectInterface interfaces={
        .deleteInstance=deleteInstance,
        .getId=getId,
        .isRun=isRun,
        .start=start,
        .stop=stop
    };
    obj->interfaces=interfaces;

    return 0;
}

static int start(void *instance, void (*worker)(void* param), void* param){

    ASSERT_NOT_NULL(instance,"start ThreadObjectWindows fail, instance is null",-1);

    ThreadObjectWindows* obj = (ThreadObjectWindows*) instance;
    ASSERT_NULL(obj->isRun,"start ThreadObjectWindows fail, thread is run",-2);

    /* 采用调用方（如 TimerExecutor）注入的 worker/param：
     * ThreadObjectInterface::start 的第 2、3 个参数才是线程体与其实参，
     * config 里的 workFunc/workParam 仅作兜底。
     * 若这里不接住 worker，线程体就会去调 config.workFunc，
     * 而组合根通常把该字段置为 NULL -> 空指针调用导致进程闪退。 */
    if (worker != NULL) {
        obj->config.workFunc  = worker;
        obj->config.workParam = param;
    }

    /* _beginthreadex 的最后一参是 unsigned*（DWORD 语义）：
     * 公共字段 id 是 unsigned long，故先经局部 unsigned 承接再回填，
     * 避免把 unsigned long* 传给 unsigned* 造成类型不匹配告警。 */
    {
        unsigned threadId = 0;
        obj->hThread = (void*)_beginthreadex(
            obj->config.safe,
            0,
            threadEntry,
            instance,
            0,
            &threadId
        );
        obj->id = (unsigned long)threadId;   /* 失败时保持 0，与原先“由 API 直接回填”等价 */
    }
    ASSERT_NOT_NULL(obj->hThread,"_beginthreadex failed",-3);

    return 0;
}
static int stop(void *instance){

    ASSERT_NOT_NULL(instance,"stop ThreadObjectWindows fail, instance is null",-1);

    ThreadObjectWindows* obj = (ThreadObjectWindows*) instance;
    ASSERT_NOT_NULL(obj->isRun,"ThreadObjectWindows is not runing",-2);

    TerminateThread((HANDLE)obj->hThread,0);
    if(obj->hThread!=NULL){ CloseHandle((HANDLE)obj->hThread); obj->hThread=NULL; }
    obj->isRun=FALSE;

    return 0;
}
static int getId(void *instance){

    ASSERT_NOT_NULL(instance,"getId ThreadObjectWindows fail, instance is null",0);
    
    ThreadObjectWindows* obj = (ThreadObjectWindows*) instance;
    ASSERT_NOT_NULL(obj->isRun,"getId fail, thread is not runing",0);

    return (int)obj->id;   /* unsigned long（等价 DWORD）-> int，显式截断转换 */
}
static int isRun(void *instance){

    ASSERT_NOT_NULL(instance,"getId ThreadObjectWindows fail, instance is null",0);
    ThreadObjectWindows* obj = (ThreadObjectWindows*) instance;

    return obj->isRun; 
}
static void deleteInstance(void *instance){
    if(instance==NULL) return;
    ThreadObjectWindows* obj = (ThreadObjectWindows*) instance;
    if(obj->isRun && obj->hThread!=NULL){
        stop(instance);          /* 仍在运行才终止线程 */
    }
    if(obj->hThread!=NULL){ CloseHandle((HANDLE)obj->hThread); obj->hThread=NULL; }
    threadObjectFree(obj);       /* 销毁具体实例（抽象包装器由 DeleteThreadObject 释放） */
}
static int isConfigIllegal(ThreadObjectWindowsConfig* config){

    return 0;
}
static void threadEntry(void* instance){
    ASSERT_NOT_NULL(instance,"threadEntry ThreadObjectWindows start fail, instance is null");

    ThreadObjectWindows* obj =(ThreadObjectWindows*)instance;
    ASSERT_NULL(obj->isRun,"threadEntry ThreadObjectWindows thread is runing");

    obj->isRun=TRUE;

    /* 线程体：由 start() 注入（见 start 内注释）；为空时只记录日志，不得直接调用 */
    if (obj->config.workFunc != NULL) {
        obj->config.workFunc(obj->config.workParam);
    } else {
        log_error("ThreadObjectWindows: workFunc is NULL, thread body skipped");
    }

    obj->isRun=FALSE;
}
