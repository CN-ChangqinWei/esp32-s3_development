#include "thread_object_macos.h"
#include <string.h>

/* 约定：本文件所有"标志位/返回码"形态的断言都显式转换为 (void*)(uintptr_t) 后再交给
 * ASSERT_NOT_NULL / ASSERT_NULL，使其与宏内的 NULL 同为指针类型比较，
 * 从而在 -Wall 下不产生"pointer/integer comparison"告警；布尔量一律 0/1。 */

/* ---- 接口函数（接口表字段：start/stop/getId/isRun/deleteInstance） ---- */
static int  start(void *instance, void (*worker)(void* param), void* param);
static int  stop(void *instance);
static int  getId(void *instance);
static int  isRun(void *instance);
static void deleteInstance(void *instance);

/* ---- 内部辅助 ---- */
static int   isConfigIllegal(ThreadObjectMacosConfig* config);
static int   isThreadAlive(pthread_t hThread);
static void* threadEntry(void* instance);

/* --------------------------------------------------------------------------
 * 具体构造：分配抽象对象 -> 调用本模块的内部装配 InitThreadObjectMacos
 * -> 把调用方配置整体落地到具体实例。
 * -------------------------------------------------------------------------- */
ThreadObject* NewThreadObjectMacos(ThreadObjectMacosConfig* config){
    ASSERT_NOT_NULL(config,"config is NULL",NULL);
    ASSERT_NULL((void*)(uintptr_t)isConfigIllegal(config),"config is illegal",NULL);

    ThreadObject* obj = threadObjectMalloc(sizeof(ThreadObject));
    ASSERT_NOT_NULL(obj,"threadObjectMalloc ThreadObject error",NULL);
    memset(obj,0,sizeof(ThreadObject));

    int err = InitThreadObjectMacos(obj);
    if(err){
        threadObjectFree(obj);
        obj=NULL;
        return obj;
    }

    /* 配置落地：把调用方传入的配置保存到具体实例（worker/start 后续依赖它）。
     * 若装配结果异常则整体回收，避免泄漏抽象对象。 */
    ThreadObjectMacos* self = (ThreadObjectMacos*)obj->instance;
    if(self==NULL){
        DeleteThreadObject(obj);
        return NULL;
    }
    self->config = *config;

    return obj;
}

/* 内部装配：分配具体实例，填好接口表，再交给抽象构造 InitThreadObject 统一装配
 * （禁止手工赋值 obj->instance / obj->interfaces）。 */
int InitThreadObjectMacos(ThreadObject* obj){
    ASSERT_NOT_NULL(obj,"InitThreadObjectMacos fail, obj is null",-1);

    ThreadObjectMacos* self = threadObjectMalloc(sizeof(ThreadObjectMacos));
    ASSERT_NOT_NULL(self,"threadObjectMalloc error",-1);
    memset(self,0,sizeof(ThreadObjectMacos));

    self->hThread    = (pthread_t)0;   /* 空句柄，stop/deleteInstance 据此判断是否需要回收 */
    self->id         = 0;
    self->isRun      = 0;
    self->isContinue = 0;

    ThreadObjectInterface interfaces={
        .deleteInstance=deleteInstance,
        .getId=getId,
        .isRun=isRun,
        .start=start,
        .stop=stop
    };
    self->interfaces=interfaces;

    int err = InitThreadObject(obj,self,interfaces);
    if(err){
        threadObjectFree(self);
        self=NULL;
        return err;
    }

    return 0;
}

static int start(void *instance, void (*worker)(void* param), void* param){

    ASSERT_NOT_NULL(instance,"start ThreadObjectMacos fail, instance is null",-1);

    ThreadObjectMacos* self = (ThreadObjectMacos*) instance;
    ASSERT_NOT_NULL((void*)(uintptr_t)self->isRun,"start ThreadObjectMacos fail, thread is run",-2);

    /* 采用调用方（如 TimerExecutor）注入的 worker/param：
     * ThreadObjectInterface::start 的第 2、3 个参数才是线程体与其实参，
     * config 里的 workFunc/workParam 仅作兜底。
     * 若这里不接住 worker，线程体就会去调 config.workFunc，
     * 而组合根通常把该字段置为 NULL -> 空指针调用导致进程闪退。 */
    if (worker != NULL) {
        self->config.workFunc  = worker;
        self->config.workParam = param;
    }

    int err = 0;
    if (self->config.stackSize > 0) {
        /* 显式指定栈大小：attr 必须在使用后 destroy，避免资源泄漏 */
        pthread_attr_t attr;
        int rc = pthread_attr_init(&attr);
        ASSERT_EQ(rc,0,"start ThreadObjectMacos fail, pthread_attr_init error",-3);

        rc = pthread_attr_setstacksize(&attr,self->config.stackSize);
        if (rc != 0) {
            pthread_attr_destroy(&attr);
            log_error("start ThreadObjectMacos fail, pthread_attr_setstacksize error");
            return -4;
        }

        err = pthread_create(&self->hThread,&attr,threadEntry,instance);
        pthread_attr_destroy(&attr);
    } else {
        /* stackSize == 0：使用平台默认栈大小 */
        err = pthread_create(&self->hThread,NULL,threadEntry,instance);
    }
    ASSERT_EQ(err,0,"start ThreadObjectMacos fail, pthread_create error",-5);

    /* macOS 下 pthread_t 为指针类型，转为 int 必须显式两段转换，避免 -Wall 告警 */
    self->id = (int)(intptr_t)self->hThread;

    return 0;
}

static int stop(void *instance){

    ASSERT_NOT_NULL(instance,"stop ThreadObjectMacos fail, instance is null",-1);

    ThreadObjectMacos* self = (ThreadObjectMacos*) instance;
    ASSERT_NOT_NULL((void*)(uintptr_t)self->isRun,"ThreadObjectMacos is not runing",-2);
    ASSERT_NOT_NULL((void*)(uintptr_t)isThreadAlive(self->hThread),"ThreadObjectMacos thread handle is invalid",-3);

    /* 语义对齐 Windows 具象的 TerminateThread（立刻停止）：
     * 1) threadEntry 入口已调用 pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL)，
     *    故 pthread_cancel 会让线程在其工作函数中的任意时刻被取消，而不必等到下一个取消点；
     *    注意：异步取消不会执行工作函数内的清理代码（不析构局部对象/不释放局部资源），
     *    这与 TerminateThread 的风险一致——调用方必须保证 worker 不长期持有共享锁等资源。
     * 2) 取消请求只是"请求"，必须 pthread_join 等待其真正退出，才能保证 join 之后
     *    不再有线程触碰本实例；join 同时回收线程栈/TLS 资源。
     * 3) 线程可能已自然结束（pthread_cancel 返回非 0，如 ESRCH），此时 join 仍然合法且必要，
     *    因此 cancel 的返回值只记录日志、不阻断流程。 */
    int rc = pthread_cancel(self->hThread);
    if (rc != 0) {
        log_error("stop ThreadObjectMacos: pthread_cancel return %d (thread maybe already exited)",rc);
    }

    pthread_join(self->hThread,NULL);

    /* 置空句柄 + 清运行标志：防止二次 join / 二次释放（后续 stop 会被 isRun 断言挡下） */
    self->hThread = (pthread_t)0;
    self->isRun = 0;

    return 0;
}

static int getId(void *instance){

    ASSERT_NOT_NULL(instance,"getId ThreadObjectMacos fail, instance is null",0);

    ThreadObjectMacos* self = (ThreadObjectMacos*) instance;
    ASSERT_NOT_NULL((void*)(uintptr_t)self->isRun,"getId fail, thread is not runing",0);

    return self->id;
}

static int isRun(void *instance){

    ASSERT_NOT_NULL(instance,"isRun ThreadObjectMacos fail, instance is null",0);
    ThreadObjectMacos* self = (ThreadObjectMacos*) instance;

    return (self->isRun)?1:0;
}

static void deleteInstance(void *instance){
    if(instance==NULL) return;
    ThreadObjectMacos* self = (ThreadObjectMacos*) instance;

    if(isThreadAlive(self->hThread)){
        if(self->isRun){
            stop(instance);          /* 仍在运行：立刻终止并回收（内部已置 hThread=0） */
        }
        /* 线程已自然结束但从未 stop：此时 isRun==0、hThread 仍有效，
         * 必须 join 回收其资源（否则线程栈/TLS 泄漏）。stop 已置空则跳过，杜绝二次 join。 */
        if(isThreadAlive(self->hThread)){
            pthread_join(self->hThread,NULL);
            self->hThread = (pthread_t)0;
        }
    }
    self->isRun = 0;
    self->isContinue = 0;

    threadObjectFree(self);       /* 销毁具体实例（抽象包装器由 DeleteThreadObject 释放） */
}

static int isConfigIllegal(ThreadObjectMacosConfig* config){
    if(config==NULL) return 1;

    /* stackSize == 0 表示使用平台默认栈大小（合法）；
     * 显式指定时不得低于平台最小栈（PTHREAD_STACK_MIN 由平台头定义，
     * 未定义时跳过该检查，不做硬编码假设）。 */
#ifdef PTHREAD_STACK_MIN
    if(config->stackSize != 0 && config->stackSize < (size_t)PTHREAD_STACK_MIN) return 1;
#endif

    return 0;
}

/* macOS(POSIX) 的 pthread_t 为空句柄时是一个空指针常量；用 p != (pthread_t)0 判定，
 * 不直接与 0 混比，避免 -Wall 下的指针/整数比较告警。 */
static int isThreadAlive(pthread_t hThread){

    return (hThread != (pthread_t)0)?1:0;
}

static void* threadEntry(void* instance){
    ASSERT_NOT_NULL(instance,"threadEntry ThreadObjectMacos start fail, instance is null",NULL);

    ThreadObjectMacos* self =(ThreadObjectMacos*)instance;

    /* 让 pthread_cancel 立即可生效（见 stop 内注释），对应 Windows 的 TerminateThread 语义 */
    int rc = pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS,NULL);
    if(rc != 0){
        log_error("ThreadObjectMacos: pthread_setcanceltype failed, rc:%d",rc);
    }

    self->isRun = 1;

    /* 线程体：由 start() 注入（见 start 内注释）；为空时只记录日志，不得直接调用 */
    if (self->config.workFunc != NULL) {
        self->config.workFunc(self->config.workParam);
    } else {
        log_error("ThreadObjectMacos: workFunc is NULL, thread body skipped");
    }

    self->isRun = 0;

    return NULL;
}
