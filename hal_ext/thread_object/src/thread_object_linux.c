#include "thread_object_linux.h"
#include <string.h>
#include <stdint.h>

static int  start(void *instance, void (*worker)(void* param), void* param);
static int  stop(void *instance);
static int  getId(void *instance);
static int  isRun(void *instance);
static void deleteInstance(void *instance);
static void* threadEntry(void* instance);

ThreadObject* NewThreadObjectLinux(ThreadObjectLinuxConfig* config){
    ASSERT_NOT_NULL(config,"NewThreadObjectLinux config is NULL",NULL);

    ThreadObject* obj = threadObjectMalloc(sizeof(ThreadObject));
    ASSERT_NOT_NULL(obj,"threadObjectMalloc ThreadObject error",NULL);
    memset(obj,0,sizeof(ThreadObject));

    int err = InitThreadObjectLinux(obj);
    if(err){
        threadObjectFree(obj);
        obj=NULL;
        return obj;
    }

    /* 配置落地：把调用方传入的配置保存到具体实例（start/worker 后续依赖它） */
    {
        ThreadObjectLinux* self = (ThreadObjectLinux*)obj->instance;
        self->config = *config;
    }

    return obj;
}
int InitThreadObjectLinux(ThreadObject* obj){
    ASSERT_NOT_NULL(obj,"InitThreadObjectLinux obj is NULL",-1);

    ThreadObjectLinux* self = threadObjectMalloc(sizeof(ThreadObjectLinux));
    ASSERT_NOT_NULL(self,"threadObjectMalloc ThreadObjectLinux error",-2);
    memset(self,0,sizeof(ThreadObjectLinux));

    ThreadObjectInterface interfaces={
        .start=start,
        .stop=stop,
        .getId=getId,
        .isRun=isRun,
        .deleteInstance=deleteInstance
    };

    /* 交由抽象构造完成装配（只设置 instance/interfaces，禁止手工赋值） */
    int err = InitThreadObject(obj,self,interfaces);
    if(err){
        threadObjectFree(self);
        self=NULL;
        return err;
    }

    return 0;
}

static int start(void *instance, void (*worker)(void* param), void* param){

    ASSERT_NOT_NULL(instance,"start ThreadObjectLinux fail, instance is null",-1);

    ThreadObjectLinux* self = (ThreadObjectLinux*)instance;
    /* hThread 非 0 或已在运行，均视为不可重复启动 */
    if(self->isRun || self->hThread!=0){
        log_error("start ThreadObjectLinux fail, thread is already run");
        return -2;
    }

    /* 采用调用方（如 TimerExecutor）注入的 worker/param，与 ThreadObjectWindows 一致：
     * ThreadObjectInterface::start 的第 2、3 个参数才是线程体与其实参，
     * config 里的 workFunc/workParam 仅作兜底。若这里不接住 worker，线程体就会去调
     * config.workFunc，而组合根通常把该字段置为 NULL -> 空指针调用导致进程闪退。 */
    if(worker != NULL){
        self->config.workFunc  = worker;
        self->config.workParam = param;
    }

    /* config.stackSize>0 时用 attr 指定栈大小。
     * 异常路径需要自行销毁 attr，故这里不使用 ASSERT 宏（宏会直接 return 造成 attr 泄漏）。 */
    pthread_attr_t  attr;
    pthread_attr_t* attrPtr = NULL;
    if(self->config.stackSize > 0){
        int attrErr = pthread_attr_init(&attr);
        if(attrErr != 0){
            log_error("start ThreadObjectLinux fail, pthread_attr_init errno:%d",attrErr);
            return -3;
        }
        attrErr = pthread_attr_setstacksize(&attr,self->config.stackSize);
        if(attrErr != 0){
            log_error("start ThreadObjectLinux fail, pthread_attr_setstacksize errno:%d",attrErr);
            pthread_attr_destroy(&attr);
            return -4;
        }
        attrPtr = &attr;
    }

    /* pthread_create 失败时句柄值未定义，先置 0 作为“未创建”哨兵 */
    self->hThread = 0;
    int err = pthread_create(&self->hThread,attrPtr,threadEntry,instance);
    if(attrPtr != NULL){
        pthread_attr_destroy(&attr);   /* 创建后即刻销毁属性，线程已独立持有自身属性副本 */
    }
    ASSERT_EQ(err,0,"start ThreadObjectLinux fail, pthread_create error",-5);

    /* pthread_t 不可移植、无法直接当 int，显式经 intptr_t 转换，避免隐式截断告警 */
    self->id = (int)(intptr_t)self->hThread;

    return 0;
}
static int stop(void *instance){

    ASSERT_NOT_NULL(instance,"stop ThreadObjectLinux fail, instance is null",-1);

    ThreadObjectLinux* self = (ThreadObjectLinux*)instance;
    /* 以 hThread 是否有效作为“需要回收”的判据：
     * 线程可能已自然结束但尚未 join，此处仍须 join 回收；
     * join 后置 0，可杜绝二次 join。 */
    if(self->hThread == 0){
        log_error("stop ThreadObjectLinux fail, thread is not running");
        return -2;
    }

    /* POSIX 无 TerminateThread，以 pthread_cancel + pthread_join 对齐
     * Windows TerminateThread 的“立刻停止”语义；线程体入口已把取消类型设为
     * 异步（见 threadEntry），故 cancel 会立即生效。 */
    pthread_cancel(self->hThread);
    pthread_join(self->hThread,NULL);

    self->hThread = 0;
    self->isRun   = 0;

    return 0;
}
static int getId(void *instance){

    ASSERT_NOT_NULL(instance,"getId ThreadObjectLinux fail, instance is null",0);

    ThreadObjectLinux* self = (ThreadObjectLinux*)instance;
    if(!self->isRun){
        log_error("getId ThreadObjectLinux fail, thread is not running");
        return 0;
    }

    return self->id;
}
static int isRun(void *instance){

    ASSERT_NOT_NULL(instance,"isRun ThreadObjectLinux fail, instance is null",0);

    ThreadObjectLinux* self = (ThreadObjectLinux*)instance;
    return self->isRun;
}
static void deleteInstance(void *instance){
    if(instance==NULL) return;
    ThreadObjectLinux* self = (ThreadObjectLinux*)instance;

    /* 仍在运行则终止；已结束则仅回收线程资源；未创建则跳过。
     * stop 内已按 hThread 防二次 join，这里无需再判 isRun。 */
    if(self->hThread!=0){
        stop(instance);
    }

    threadObjectFree(self);   /* 销毁具体实例（抽象包装器由 DeleteThreadObject 释放） */
}
static void* threadEntry(void* instance){
    ASSERT_NOT_NULL(instance,"threadEntry ThreadObjectLinux fail, instance is null",NULL);

    ThreadObjectLinux* self = (ThreadObjectLinux*)instance;

    /* 设为异步取消，使 stop() 的 pthread_cancel 立即生效，
     * 对齐 Windows TerminateThread 的“立刻停止”语义。
     * 注意：异步取消可能在任意指令处打断线程体，若线程体持有锁或处于临界区，
     * 取消时不会自动释放，需由调用方自行权衡；本模块只对齐停止语义。 */
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS,NULL);

    self->isRun = 1;

    /* 线程体：由 start() 注入（见 start 内注释）；为空时仅记录日志，绝不空指针调用 */
    if(self->config.workFunc != NULL){
        self->config.workFunc(self->config.workParam);
    }else{
        log_error("ThreadObjectLinux: workFunc is NULL, thread body skipped");
    }

    self->isRun = 0;

    return NULL;
}
