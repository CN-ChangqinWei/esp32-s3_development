#include"timer_executor.h"

void* (*timerExecutorMalloc)(size_t size)=malloc;
void  (*timerExecutorFree)(void* p)=free;

static int start(TimerExecutor* ins,uint64_t interval);
static int pause(TimerExecutor* ins);
static int stop(TimerExecutor* ins);
static int isRun(TimerExecutor* ins);
static int isConfigIlleage(TimerExecutorConfig* config);
static int conti(TimerExecutor* ins);
static void DeleteConfig(TimerExecutorConfig* config);
static void worker(void* ins);

TimerExecutor* NewTimerExecutor(TimerExecutorConfig* config){
    //检查config合法性
    ASSERT_NOT_NULL(config,"config is null",NULL);
    ASSERT_EQ(isConfigIlleage(config),0,"config illeage",NULL);

    //申请内存
    TimerExecutor* ins = timerExecutorMalloc(sizeof(TimerExecutor));
    ASSERT_NOT_NULL(ins,"malloc mem is NULL",NULL);
    
    //初始化参数
    if(InitTimerExecutor(ins,config)){
        timerExecutorFree(ins);
        return NULL;
    }

    //返回指针
    return ins;
}
void DeleteTimerExecutor(TimerExecutor* ins){
    ASSERT_NOT_NULL(ins,"ins is NULL");
    DeleteConfig(&ins->config);
    timerExecutorFree(ins);
}
int  InitTimerExecutor(TimerExecutor* ins,TimerExecutorConfig* config){
    ASSERT_NOT_NULL(ins,"ins is NULL ",1);
    ASSERT_EQ(isConfigIlleage(config),0,"config is illeage",1);

    ins->config=*config;
    ins->_isRun=0;
    ins->_suicide=0;
    ins->_lastTime=0;
    ins->isRun=isRun;
    ins->start=start;
    ins->pause=pause;
    ins->stop=stop;
    ins->conti=conti;
    

    return 0;
}

static void DeleteConfig(TimerExecutorConfig* config){
    ASSERT_NOT_NULL(config,"config is NULL");
    ASSERT_NOT_NULL(config->threadObj,"config-> threadObj is NULL");
    DeleteThreadObject(config->threadObj);
}

static int isConfigIlleage(TimerExecutorConfig* config){

    ASSERT_NOT_NULL(config,"config is NULL",1);
    ASSERT_NOT_NULL(config->threadObj,"config-> threadObj is NULL",1);
    ASSERT_NOT_NULL(config->event,"config->event is NULL",1);
    ASSERT_NOT_NULL(config->getTimestamp,"config->getTimestamp is NULL",1);
    ASSERT_NOT_NULL(config->sleep,"config->sleep is NULL",1);

    return 0;
}
static int start(TimerExecutor* ins,uint64_t interval){
    ASSERT_NOT_NULL(ins,"ins is null",-1);
    ASSERT_GREATER(interval,1,"interval must greater than 1 at least",-2);
    ASSERT_EQ(isConfigIlleage(&ins->config),0,"timerExecutor config  illeage",-3);

    ins->_interval=interval;
    ins->_sleepTime=interval/2;
    ins->_isPause=0;

    if(ins->_isRun) return 0;
    //start thread obj
    ThreadObject* threadObj = ins->config.threadObj;
    ASSERT_EQ(
        threadObj->interfaces.start((void*)threadObj->instance,worker,(void*)ins),
        0,
        "timerExecutor start thread obj failed",
        -4
    );
    return 0;
}
static int pause(TimerExecutor* ins){
    ASSERT_NOT_NULL(ins,"ins is NULL",-1);
    ins->_isPause=1;
    return 0;
}
static int conti(TimerExecutor* ins){
    ASSERT_NOT_NULL(ins,"ins is NULL",-1);
    ins->_isPause=0;
    return 0;
}
static int stop(TimerExecutor* ins){
    ASSERT_NOT_NULL(ins,"ins is NULL",-1);
    ins->_suicide=1;
    return 0;
}
static int isRun(TimerExecutor* ins){
    ASSERT_NOT_NULL(ins,"ins is NULL",-1);
    return ins->_isRun;
}

static void worker(void* ins){
    ASSERT_NOT_NULL(ins,"ins is null");
    TimerExecutor* timerExecutor = (TimerExecutor*)ins;
    ASSERT_EQ(isConfigIlleage(&timerExecutor->config),0,"timerExecutor config  illeage");

    void (*timerSleep)(uint64_t) = timerExecutor->config.sleep;
    //开始工作，标志位设置
    timerExecutor->_isRun=1;
    uint64_t (*getTimestamp)() = timerExecutor->config.getTimestamp;
    uint64_t lastTime = getTimestamp();
    uint64_t currentTime = getTimestamp();
    uint64_t deltaTime = 0;
    uint64_t * timeCnt = &timerExecutor->_timeCnt;
    uint64_t * interval = &timerExecutor->_interval;
    uint64_t * sleepTime=&timerExecutor->_sleepTime;

    *timeCnt=0;
    //循环执行,自杀值为真的时候退出
    
    while (0==timerExecutor->_suicide)
    {
        /* code */
        if(0==timerExecutor->_isPause){
            currentTime=getTimestamp();
            deltaTime = currentTime - lastTime;
            *timeCnt+=deltaTime;
            if(*timeCnt>=*interval){
                timerExecutor->config.event(timerExecutor->config.param);
                *timeCnt=0;
            }
        }else{
            lastTime=getTimestamp();
        }
        timerSleep(*sleepTime);
    }
    
    //结束，isRun为false
    timerExecutor->_isRun=0;
    timerExecutor->_suicide=0;
}
