#include "info_service.h"

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>

#include "sdkconfig.h"     /* CONFIG_COMPILER_OPTIMIZATION_DEBUG：debug / release 分支开关 */
#include "esp_log.h"

#include "app_types.h"
#include "event_loop.h"

/* ---------------------------------------------------------------------
 * debug / release 分支开关
 *
 * debug（CONFIG_COMPILER_OPTIMIZATION_DEBUG == 1）：本模块自带定时线程，
 *   周期性地把回传任务投递到 UART 线程；
 * release：既不建线程也不建定时器，只有访问服务（InfoServiceRequestPush）
 *   时才回传一次。release 分支连 timer_executor.h / thread_object.h /
 *   platform.h 都不包含，确保不引用这两个模块的任何符号，避免把不需要的
 *   代码链进固件。
 * ------------------------------------------------------------------- */
#if defined(CONFIG_COMPILER_OPTIMIZATION_DEBUG) && CONFIG_COMPILER_OPTIMIZATION_DEBUG
#define INFO_SERVICE_TIMER_ENABLE 1
#else
#define INFO_SERVICE_TIMER_ENABLE 0
#endif

#if INFO_SERVICE_TIMER_ENABLE
#include "timer_executor.h"   /* TimerExecutor / TimerExecutorConfig */
#include "thread_object.h"    /* ThreadObject / DeleteThreadObject */
#include "platform.h"         /* NewPlatformThreadObject / NewPlatformTimestamp / NewPlatformSleep */
#endif

/* 模块级内存接口（依赖反转）：默认指向 malloc / free，可由装配区整体替换。
 * 未在公共头声明：公共头的 include 白名单不含 <stddef.h>，需要替换时在装配文件内
 * 自行 extern 声明这两个符号后赋值即可（二者均为外部链接）。 */
void* (*infoServiceMalloc)(size_t size) = malloc;
void  (*infoServiceFree)(void* p) = free;

static const char* INFO_SERVICE_TAG = "info_service";

/* debug 定时线程参数（需求：栈 3072、优先级 4；FreeRTOS 下栈单位是「字」） */
#define INFO_SERVICE_THREAD_STACK     3072
#define INFO_SERVICE_THREAD_PRIORITY  4
#define INFO_SERVICE_THREAD_NAME      "info_timer"

/* 默认回传周期（需求：1 s） */
#define INFO_SERVICE_DEFAULT_PERIOD_MS  APP_INFO_PUSH_PERIOD_MS

struct InfoService {
    EventLoop*         loop;             /* 注入：回传任务的执行线程（借用，不释放） */
    InfoSampleProvider provider;         /* 注入：取当前快照 */
    void*              providerContext;  /* 注入：provider 的上下文（借用，不释放） */
    uint32_t           pushPeriodMs;     /* 已归一化的回传周期（毫秒） */

#if INFO_SERVICE_TIMER_ENABLE
    /* 定时线程：装配成功后其所有权随 TimerExecutor 一并移交（见 DeleteInfoService 注释） */
    ThreadObject*  timerThread;
    TimerExecutor* timer;                /* 周期定时器：debug 分支唯一的时间源 */
#endif
};

/* 周期归一化：0 表示「用默认值」；1 ms 周期对日志回传没有意义，且 TimerExecutor
 * 要求 interval > 1，故 0 / 1 一律回落到默认周期。 */
static uint32_t infoServiceNormalizePeriodMs(uint32_t periodMs)
{
    return (periodMs > 1u) ? periodMs : (uint32_t)INFO_SERVICE_DEFAULT_PERIOD_MS;
}

/* ---------------------------------------------------------------------
 * 回传动作：在注入的 EventLoop 线程内执行，因此串口输出只发生在一个线程里。
 * ------------------------------------------------------------------- */
static void infoServicePushJob(void* param)
{
    InfoService* self = (InfoService*)param;
    AccelSample  sample;

    if (self == NULL) {
        return;
    }
    if (self->provider == NULL) {
        ESP_LOGW(INFO_SERVICE_TAG, "sample provider is null, push skipped");
        return;
    }

    /* 先清零：provider 未填充的字段按 0 / valid=0 输出，避免打印栈上杂值 */
    sample.xMg         = 0;
    sample.yMg         = 0;
    sample.zMg         = 0;
    sample.timestampMs = 0u;
    sample.valid       = (uint8_t)0u;

    self->provider(self->providerContext, &sample);

    ESP_LOGI(INFO_SERVICE_TAG, "accel x=%ld y=%ld z=%ld t=%lu valid=%u mg",
             (long)sample.xMg, (long)sample.yMg, (long)sample.zMg,
             (unsigned long)sample.timestampMs, (unsigned)sample.valid);
}

int InfoServiceRequestPush(InfoService* service)
{
    int err;

    if (service == NULL) {
        ESP_LOGE(INFO_SERVICE_TAG, "request push: service is null");
        return -1;
    }
    if ((service->loop == NULL) || (service->loop->instance == NULL) ||
        (service->loop->interfaces.post == NULL)) {
        ESP_LOGE(INFO_SERVICE_TAG, "request push: injected event loop is invalid");
        return -1;
    }

    err = service->loop->interfaces.post(service->loop->instance,
                                         infoServicePushJob,
                                         (void*)service);
    if (err != 0) {
        /* 队列已满 / 循环未运行：本次回传丢弃，绝不阻塞调用者（尤其定时线程） */
        ESP_LOGW(INFO_SERVICE_TAG, "request push: post failed, err=%d", err);
        return -2;
    }

    return 0;
}

#if INFO_SERVICE_TIMER_ENABLE
/* 时间源包装：TimerExecutorConfig.getTimestamp 为无参函数，这里取平台毫秒时间戳。
 * NewPlatformTimestamp 只返回一组函数指针（不分配内存），每次取用无额外代价。 */
static uint64_t infoServiceTimestampMs(void)
{
    PlatformTimestampInterface timestampIf = NewPlatformTimestamp();

    return (timestampIf.milliseconds != NULL) ? timestampIf.milliseconds() : (uint64_t)0u;
}

/* 睡眠包装：TimerExecutorConfig.sleep 以毫秒为单位，这里转调平台毫秒睡眠 */
static void infoServiceSleepMs(uint64_t ms)
{
    PlatformSleepInterface sleepIf = NewPlatformSleep();

    if ((sleepIf.sleepMs != NULL) && (ms > 0u)) {
        sleepIf.sleepMs((uint32_t)ms);
    }
}

/* 定时回调：运行在定时线程，只把回传任务投递到 UART 线程，
 * 绝不在这里打印（保证串口输出只在一个线程里发生）。 */
static void infoServiceTimerEvent(void* param)
{
    InfoService* self = (InfoService*)param;

    if (self == NULL) {
        return;
    }

    (void)InfoServiceRequestPush(self);
}

/* 装配 debug 专用资源：定时线程 + 周期定时器。
 * 返回 0 成功；-1 线程创建失败；-2 定时器创建失败（资源已回滚）。 */
static int infoServiceCreateTimer(InfoService* self)
{
    PlatformThreadConfig threadConfig;
    TimerExecutorConfig  timerConfig;

    threadConfig.stackSize = INFO_SERVICE_THREAD_STACK;
    threadConfig.priority  = INFO_SERVICE_THREAD_PRIORITY;
    threadConfig.name      = INFO_SERVICE_THREAD_NAME;
    threadConfig.workFunc  = NULL;   /* 线程体与实参由 TimerExecutor 在 start 时注入 */
    threadConfig.workParam = NULL;

    self->timerThread = NewPlatformThreadObject(&threadConfig);
    if (self->timerThread == NULL) {
        ESP_LOGE(INFO_SERVICE_TAG, "create timer thread object failed (out of memory)");
        return -1;
    }

    timerConfig.event        = infoServiceTimerEvent;
    timerConfig.param        = (void*)self;
    timerConfig.threadObj    = self->timerThread;
    timerConfig.getTimestamp = infoServiceTimestampMs;
    timerConfig.sleep        = infoServiceSleepMs;

    self->timer = NewTimerExecutor(&timerConfig);
    if (self->timer == NULL) {
        /* 定时器未装配成功：ThreadObject 所有权尚未移交，由本模块自行回收 */
        ESP_LOGE(INFO_SERVICE_TAG, "create timer executor failed");
        DeleteThreadObject(self->timerThread);
        self->timerThread = NULL;
        return -2;
    }

    return 0;
}
#endif /* INFO_SERVICE_TIMER_ENABLE */

InfoService* NewInfoService(const InfoServiceConfig* config)
{
    InfoService* self;

    if (config == NULL) {
        ESP_LOGE(INFO_SERVICE_TAG, "NewInfoService: config is null");
        return NULL;
    }
    if ((config->loop == NULL) || (config->loop->interfaces.post == NULL)) {
        ESP_LOGE(INFO_SERVICE_TAG, "NewInfoService: config->loop is null or has no post");
        return NULL;
    }
    if (config->provider == NULL) {
        ESP_LOGE(INFO_SERVICE_TAG, "NewInfoService: config->provider is null");
        return NULL;
    }

    self = infoServiceMalloc(sizeof(InfoService));
    if (self == NULL) {
        ESP_LOGE(INFO_SERVICE_TAG, "NewInfoService: out of memory");
        return NULL;
    }

    /* 逐字段初始化（不整体清零：本模块不引入 <string.h>，保持头文件白名单干净） */
    self->loop            = config->loop;
    self->provider        = config->provider;
    self->providerContext = config->providerContext;
    self->pushPeriodMs    = infoServiceNormalizePeriodMs(config->pushPeriodMs);

#if INFO_SERVICE_TIMER_ENABLE
    self->timerThread = NULL;
    self->timer       = NULL;
    if (infoServiceCreateTimer(self) != 0) {
        infoServiceFree(self);
        return NULL;
    }
#endif

    return self;
}

void DeleteInfoService(InfoService** pService)
{
    InfoService* self;

    if (pService == NULL) {
        return;
    }
    self = *pService;
    if (self == NULL) {
        return;
    }

#if INFO_SERVICE_TIMER_ENABLE
    if (self->timer != NULL) {
        /* 1) 先请求定时线程退出，避免它在本对象释放后继续投递任务 */
        self->timer->stop(self->timer);

        /* 2) 注意：DeleteTimerExecutor 内部会执行 DeleteThreadObject（见
         *    timer_executor.c 的 DeleteConfig），即定时线程的所有权属于
         *    TimerExecutor。这里【不能】再 DeleteThreadObject 一次，
         *    否则线程实例会被二次释放；线程若仍在睡眠，由 ThreadObject 具象的
         *    deleteInstance 强制销毁兜底。 */
        DeleteTimerExecutor(self->timer);
        self->timer       = NULL;
        self->timerThread = NULL;
    } else if (self->timerThread != NULL) {
        /* 定时器装配失败时的兜底：此时所有权仍在本模块手上 */
        DeleteThreadObject(self->timerThread);
        self->timerThread = NULL;
    }
#endif /* INFO_SERVICE_TIMER_ENABLE */

    /* 注入的 loop / provider / providerContext 均为借用，一律不释放 */
    infoServiceFree(self);
    *pService = NULL;
}

int InfoServiceStart(InfoService* service)
{
    if (service == NULL) {
        ESP_LOGE(INFO_SERVICE_TAG, "start: service is null");
        return -1;
    }

#if INFO_SERVICE_TIMER_ENABLE
    if (service->timer == NULL) {
        ESP_LOGE(INFO_SERVICE_TAG, "start: timer is not available");
        return -2;
    }

    if (service->timer->start(service->timer, (uint64_t)service->pushPeriodMs) != 0) {
        ESP_LOGE(INFO_SERVICE_TAG, "start: timer executor start failed, period=%u ms",
                 (unsigned)service->pushPeriodMs);
        return -1;
    }

    ESP_LOGI(INFO_SERVICE_TAG, "info push timer started, period=%u ms, print on uart thread",
             (unsigned)service->pushPeriodMs);
    return 0;
#else
    /* release：只有访问服务（InfoServiceRequestPush）时才回传，定时器不可用；
     * 返回 0 以便装配代码无需区分构建模式。 */
    ESP_LOGD(INFO_SERVICE_TAG, "start: timer disabled (release build), push on demand only");
    return 0;
#endif
}
