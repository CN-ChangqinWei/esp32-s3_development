#include"thread_object_freertos.h"
#include<string.h>

/* stackDepth <= 0 时的栈深兜底值（单位：字，而非字节）。 */
#define DEFAULT_STACK_DEPTH (1024)
/* config.name 为 NULL 时的默认任务名。 */
#define DEFAULT_TASK_NAME   "thread_object_ftos"

static int start(void *instance, void (*worker)(void* param), void* param);
static int stop(void *instance);
static int getId(void *instance);
static int isRun(void *instance);
static void deleteInstance(void *instance);
static void threadEntry(void* instance);

ThreadObject* NewThreadObjectFreertos(ThreadObjectFreertosConfig* config){
    ASSERT_NOT_NULL(config,"config is NULL",NULL);

    ThreadObject* obj = threadObjectMalloc(sizeof(ThreadObject));
    ASSERT_NOT_NULL(obj,"threadObjectMalloc ThreadObject error",NULL);
    memset(obj,0,sizeof(ThreadObject));

    int err = InitThreadObjectFreertos(obj);
    if(err){
        threadObjectFree(obj);
        obj=NULL;
        return obj;
    }

    /* 配置落地：把调用方传入的配置保存到具体实例（start/threadEntry 后续依赖它） */
    {
        ThreadObjectFreertos* self = (ThreadObjectFreertos*)obj->instance;
        self->config = *config;
    }

    return obj;
}
int InitThreadObjectFreertos(ThreadObject* obj){
    ASSERT_NOT_NULL(obj,"obj is NULL",-1);

    /* 具体实例内存一律走注入式 threadObjectMalloc，禁止直接用 pvPortMalloc/malloc */
    ThreadObjectFreertos* self = threadObjectMalloc(sizeof(ThreadObjectFreertos));
    ASSERT_NOT_NULL(self,"threadObjectMalloc error",-1);
    memset(self,0,sizeof(ThreadObjectFreertos));

    obj->instance=self;
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

    ASSERT_NOT_NULL(instance,"start ThreadObjectFreertos fail, instance is null",-1);

    ThreadObjectFreertos* self = (ThreadObjectFreertos*) instance;
    /* isRun 为 char（非指针）：原 ASSERT_NULL 展开为 NULL!=(isRun)，会触发
     * "comparison between pointer and integer" 告警；此处改为等价的显式整型
     * 判断（isRun!=0 即已运行），失败路径与原宏逐字一致（同日志文案 / 同返回值）。 */
    if (0 != self->isRun) {
        log_error("assert: except  null but not null; msg:%s","start ThreadObjectFreertos fail, thread is run");
        return -2;
    }
    ASSERT_NULL(self->hThread,"start ThreadObjectFreertos fail, thread handle is not null",-3);

    /* 采用调用方（如 TimerExecutor）注入的 worker/param：
     * ThreadObjectInterface::start 的第 2、3 个参数才是线程体与其实参，
     * config 里的 workFunc/workParam 仅作兜底。
     * 若这里不接住 worker，线程体就会去调 config.workFunc，
     * 而组合根通常把该字段置为 NULL -> 空指针调用导致崩溃。 */
    if (worker != NULL) {
        self->config.workFunc  = worker;
        self->config.workParam = param;
    }

    /* 栈深按字计；<=0 时退回默认值，避免把非法深度透传给 xTaskCreate */
    configSTACK_DEPTH_TYPE stackDepth =
        (self->config.stackDepth > 0) ? (configSTACK_DEPTH_TYPE)self->config.stackDepth
                                      : (configSTACK_DEPTH_TYPE)DEFAULT_STACK_DEPTH;
    const char* taskName =
        (self->config.name != NULL) ? self->config.name : DEFAULT_TASK_NAME;

    /* 任务体为自己实现的 threadEntry；句柄回填到 self->hThread */
    BaseType_t ret = xTaskCreate(
        threadEntry,
        taskName,
        stackDepth,
        instance,
        (UBaseType_t)self->config.priority,
        &self->hThread
    );
    if (ret != pdPASS) {
        log_error("start ThreadObjectFreertos fail, xTaskCreate ret:%d",(int)ret);
        self->hThread = NULL;
        return -4;
    }

    /* 句柄降位为整型标识，供 getId 返回（语义见头文件注释） */
    self->id = (int)(uintptr_t)self->hThread;

    return 0;
}
static int stop(void *instance){

    ASSERT_NOT_NULL(instance,"stop ThreadObjectFreertos fail, instance is null",-1);

    ThreadObjectFreertos* self = (ThreadObjectFreertos*) instance;
    ASSERT_NOT_NULL(self->hThread,"stop ThreadObjectFreertos fail, thread is not runing",-2);

    vTaskDelete(self->hThread);
    self->hThread = NULL;   /* 置空防悬空/重复删除 */
    self->isRun = 0;

    return 0;
}
static int getId(void *instance){

    ASSERT_NOT_NULL(instance,"getId ThreadObjectFreertos fail, instance is null",0);

    ThreadObjectFreertos* self = (ThreadObjectFreertos*) instance;

    return self->id;
}
static int isRun(void *instance){

    ASSERT_NOT_NULL(instance,"isRun ThreadObjectFreertos fail, instance is null",0);

    ThreadObjectFreertos* self = (ThreadObjectFreertos*) instance;

    return self->isRun;
}
static void deleteInstance(void *instance){
    if(instance==NULL) return;
    ThreadObjectFreertos* self = (ThreadObjectFreertos*) instance;

    if(self->isRun && self->hThread!=NULL){
        stop(instance);          /* 仍在运行才删除任务（stop 内部会把句柄置 NULL） */
    }
    threadObjectFree(self);      /* 销毁具体实例（抽象包装器由 DeleteThreadObject 释放） */
}
static void threadEntry(void* instance){

    ASSERT_NOT_NULL(instance,"threadEntry ThreadObjectFreertos start fail, instance is null");

    ThreadObjectFreertos* self = (ThreadObjectFreertos*) instance;
    self->isRun = 1;

    /* 线程体：由 start() 注入（见 start 内注释）；为空时只记录日志，严禁空指针调用 */
    if (self->config.workFunc != NULL) {
        self->config.workFunc(self->config.workParam);
    } else {
        log_error("ThreadObjectFreertos: workFunc is NULL, thread body skipped");
    }

    self->isRun = 0;

    /* 任务体结束前自删，避免任务返回后残留；句柄一并置空防悬空 */
    self->hThread = NULL;
    vTaskDelete(NULL);
}
