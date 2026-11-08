/* =====================================================================
 * 【必须】让本编译单元内包含的 SDK 头生成 5 个 GUID / PROPERTYKEY 的**定义**
 * （而不仅是声明），以消除链接期 undefined reference：
 *     CLSID_MMDeviceEnumerator     IID_IMMDeviceEnumerator
 *     IID_IAudioClient             IID_IAudioCaptureClient
 *     PKEY_Device_FriendlyName
 *
 * 原因（已用 nm 逐个确证）：MinGW 13.1 的 libmmdevapi.a / libuuid.a /
 *   libpropsys.a 中**都不存在**上述符号，故仅靠
 *   -lmmdevapi -luuid -lpropsys **无法**解决该链接错误。
 *   而 MinGW 的 <guiddef.h>:59 / <propkeydef.h>:33 在 INITGUID 已定义时，
 *   会把 DEFINE_GUID / DEFINE_PROPERTYKEY 展开为 DECLSPEC_SELECTANY 的**定义**。
 *
 * 作用域：本宏只影响本编译单元；生成的符号带 selectany（弱符号 / COMDAT），
 *   即便其它 TU 也定义同名 GUID 亦不冲突 —— 故全工程仅此一处需要它。
 * 位置：必须早于**所有** #include（尤其早于 <windows.h>，因为 <windows.h> 会先
 *   包含 <guiddef.h>，后者一旦展开便锁定 DEFINE_GUID 的形态）。
 * ===================================================================== */
#define INITGUID

#include "platform_audio_capture.h"
#include "assert_marco.h"
#include <string.h>
#include <stddef.h>

/* =====================================================================
 * platform_audio_capture —— 跨平台「音频采集」能力实现（对象式）
 *
 * 结构（自顶向下，与 platform_screencapture.c 分段保持一致）：
 *   1) 能力开关 PLATFORM_USE_AUDIO_CAPTURE：关闭时不生成任何函数；
 *   2) 抽象层（与平台无关，能力开启即无条件生成）：
 *        NewPlatformAudioCapture / InitPlatformAudioCapture / DeletePlatformAudioCapture；
 *   3) 平台互斥分支（同一编译单元只命中一个）：
 *        PLATFORM_WINDOWS -> WASAPI 具象（独立采集线程 + 环形缓冲 + 内部转码）
 *                            + NewPlatformAudioCaptureNative
 *        #else（Linux/macOS/FreeRTOS）-> NewPlatformAudioCaptureNative 返回 NULL（可链接）
 *
 * 内存：整个模块的堆分配/释放统一走同模块能力 NewPlatformMemory()
 *       （见 platform_memory.h）；若编译期关闭了内存能力（PLATFORM_USE_MEMORY==0），
 *       为不与本文件的自依赖冲突，退回 <stdlib.h> 的 malloc/free，并在此注明原因。
 *
 * 平台隔离：具体实现结构体（PlatformAudioCaptureWindows）与本文件用到的全部 COM /
 *       Windows 头（<windows.h> / <mmdeviceapi.h> / <audioclient.h> / ...）**只**出现在
 *       本 .c 的 Windows 分支内，绝不写进 platform_audio_capture.h —— 该头会被 C++ 的
 *       Qt 具象层 / demo 间接包含，一旦引入 <windows.h>，其 COM 头里的
 *       `#define interface struct` 与 IN/OUT/TRUE/FALSE 等宏会污染整个工程，并可能与
 *       本头中形如 `interfaces` 的字段名冲突。这是对「具体实现结构体放头文件」规范的
 *       **有意偏离**（与 platform_screencapture.h / platform_network.h 一致）。
 *
 * ---------------------------------------------------------------------
 * 可观测性（本轮新增；**不改变任何行为、控制流与返回值**）
 * ---------------------------------------------------------------------
 *   本模块链路上有两类「按设计就是合法返回 0」的情况，会让现场表现为
 *   「既无识别输出、也一条 error 都没有」：
 *     ① read() 因「可用字节数 < minChunkBytes」返回 0（头文件契约：这不是错误）；
 *     ② 「采到 0 字节」与「采到静音包」（AUDCLNT_BUFFERFLAGS_SILENT）。
 *   为此在实现内补齐以下观测点，**全部经节流**（见 AUDIOCAP_LOG_INTERVAL_MS /
 *   AUDIOCAP_IDLE_WARN_MS），既有控制流与返回值逐字未改：
 *     - 采集线程周期「写入统计」：写入字节 / 包数 / 静音包数 / 环形缓冲水位；
 *     - 连续 AUDIOCAP_IDLE_WARN_MS 取不到任何包时打 warn，并附
 *       IAudioClient_GetCurrentState() 的端点状态（0=UNINITIALIZED 1=ACTIVE
 *       2=INACTIVE 3=SUSPENDED），以区分「系统没出声音」与「端点挂起 / 全静音」；
 *     - read() 交不出货时的**严格节流** info（消费者每 100 ms 调一次，绝不刷屏）；
 *     - 转码链路上 3 处「本该产出数据却产出 0 字节」的分支补 log（见 audioCapConvert
 *       的非法配置 / 缓冲写满，以及 audioCapHandlePacket 的「frames>0 但 0 字节产出」）。
 *   另：lastError 此前是**死字段**（构造时置 0，此后从未被写入，接口上也无读取途径，
 *   故 HRESULT 永远浮不到 read() 的调用方）。本轮把它改为「每个失败点写入可读文本」
 *   + 虚表读取槽 lastError（契约同 platform_websocket.h），写入点见 audioCapSetErrorf 调用处。
 * ===================================================================== */

#if !PLATFORM_USE_AUDIO_CAPTURE

/* 能力关闭：不生成任何函数。
 * platform_audio_capture.h 中的 typedef 仍然存在（本编译单元非空），无需额外占位。 */

#else /* PLATFORM_USE_AUDIO_CAPTURE */

/* ---------------------------------------------------------------------
 * 内存：统一走同模块能力 NewPlatformMemory()
 * ------------------------------------------------------------------- */
#if PLATFORM_USE_MEMORY

static void* audioCaptureMalloc(size_t size)
{
    PlatformMemoryInterface mem = NewPlatformMemory();
    return mem.malloc(size);
}

static void audioCaptureFree(void* ptr)
{
    PlatformMemoryInterface mem = NewPlatformMemory();
    mem.free(ptr);
}

#else /* PLATFORM_USE_MEMORY == 0 */

/* 内存能力被裁剪时 NewPlatformMemory 不可用。本模块必须能分配抽象对象与具体
 * 实例，故按规范允许的例外退回标准库 malloc/free（仅此一处，且为唯一退路）。 */
#include <stdlib.h>

static void* audioCaptureMalloc(size_t size)
{
    return malloc(size);
}

static void audioCaptureFree(void* ptr)
{
    free(ptr);
}

#endif /* PLATFORM_USE_MEMORY */

/* =====================================================================
 * 抽象层（与平台无关，能力开启即无条件生成）
 * ===================================================================== */

PlatformAudioCapture* NewPlatformAudioCapture(void* instance, PlatformAudioCaptureInterface interfaces)
{
    PlatformAudioCapture* capture;

    ASSERT_NOT_NULL(instance, "NewPlatformAudioCapture: instance is null", NULL);

    capture = (PlatformAudioCapture*)audioCaptureMalloc(sizeof(PlatformAudioCapture));
    ASSERT_NOT_NULL(capture, "NewPlatformAudioCapture: audioCaptureMalloc failed", NULL);

    /* 交由 Init 完成装配（只设置 instance/interfaces，禁止手工赋值） */
    if (InitPlatformAudioCapture(capture, instance, interfaces) != 0) {
        audioCaptureFree(capture);
        capture = NULL;
        return capture;
    }

    return capture;
}

int InitPlatformAudioCapture(PlatformAudioCapture* capture, void* instance,
                             PlatformAudioCaptureInterface interfaces)
{
    ASSERT_NOT_NULL(capture, "InitPlatformAudioCapture: capture is null", -1);
    ASSERT_NOT_NULL(instance, "InitPlatformAudioCapture: instance is null", -2);

    capture->instance   = instance;
    capture->interfaces = interfaces;

    return 0;
}

void DeletePlatformAudioCapture(PlatformAudioCapture** pCapture)
{
    PlatformAudioCapture* capture;

    if (pCapture == NULL) {
        return;
    }

    capture = *pCapture;
    if (capture == NULL) {
        return;
    }

    /* 先销毁具体实例（传具体实例，不是抽象包装对象） */
    if (capture->interfaces.deleteInstance != NULL) {
        capture->interfaces.deleteInstance(capture->instance);
        capture->instance = NULL;
    }

    /* 再释放抽象对象 */
    audioCaptureFree(capture);

    /* 最后置空调用方指针，防悬空 */
    *pCapture = NULL;
}

/* =====================================================================
 * 平台具象：Windows（WASAPI 系统环回 / 麦克风）
 * ===================================================================== */
#if defined(PLATFORM_WINDOWS)

/* ---------------------------------------------------------------------
 * Windows 平台头与 COM 调用路线（仅此分支包含平台头）
 *
 * COM 调用路线：CINTERFACE + COBJMACROS。
 *   在包含平台头之前 `#define CINTERFACE` 让 MinGW-w64 的头文件以纯 C 结构体 +
 *   函数指针 vtable 的形式声明 COM 接口，并生成 `IMMDeviceEnumerator_EnumAudioEndpoints(...)`
 *   这类 C 可调用的 objc-macro；`#define COBJMACROS` 让上述宏展开为
 *   `指针->lpVtbl->Method(指针, ...)` 的调用形式。这样本 .c 是**纯 C（C11）**、
 *   零 C++、零 Qt，也不需要手写 COM vtable。
 * ------------------------------------------------------------------- */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef CINTERFACE
#define CINTERFACE
#endif
#ifndef COBJMACROS
#define COBJMACROS
#endif

#include <windows.h>
#include <mmreg.h>
#include <objbase.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <propsys.h>
#include <functiondiscoverykeys_devpkey.h>
#include <stdarg.h>    /* lastError 文本写入（audioCapSetErrorf 可变参） */
#include <stdio.h>     /* lastError 文本写入（vsnprintf） */

/* 老版本 MinGW 头可能缺少 AUTOCONVERT 相关常量：给出兼容兜底定义（值取自 SDK 头）。 */
#ifndef AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM 0x80000000
#endif
#ifndef AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#endif
#ifndef AUDCLNT_E_UNSUPPORTED_FORMAT
#define AUDCLNT_E_UNSUPPORTED_FORMAT ((HRESULT)0x88890008L)
#endif

/* ---------------------------------------------------------------------------
 * MinGW-w64 的 <audioclient.h> 未实现 IAudioClient::GetCurrentState（Windows 7 新增）。
 * 已核对本机 mingw1310_64 的 audioclient.h：IAudioClientVtbl 末项是 GetService
 * （其后 END_INTERFACE），全头 0 处 GetCurrentState / AUDIOCLIENT_STATE。
 * 因此这不是「缺包含」能解决的问题，只能按 COM vtbl 的稳定 ABI 契约自行补一条：
 *
 *   IUnknown 3 项(idx 0..2) + IAudioClient 12 项：
 *     Initialize=3 GetBufferSize=4 GetStreamLatency=5 GetCurrentPadding=6
 *     IsFormatSupported=7 GetMixFormat=8 GetDevicePeriod=9
 *     Start=10 Stop=11 Reset=12 SetEventHandle=13 GetService=14
 *   ⇒ GetCurrentState = idx 15（紧随 GetService，Windows 7 新增）；
 *     其后 GetSharedModeEngine（Windows 8 新增）才是 idx 16，**不要错位一格**。
 *
 * 前提：GetCurrentState 自 Windows 7 起存在。若运行在 Windows Vista（真实 vtbl 仅 idx 0..14），
 * 按 idx 15 取将读到 vtbl 之外 —— 本工程实际目标为 Windows 10+，且调用点已用 FAILED() 兜底。
 *
 * ⚠️ 刻意**不**定义 AUDIOCLIENT_STATE 类型：MinGW 没有它，但换用 MSVC SDK 时它会被定义，
 * 自定义同名 typedef 会造成重定义冲突。故状态值一律用 int 承载，
 * 枚举含义（0=UNINITIALIZED 1=ACTIVE 2=INACTIVE 3=SUSPENDED）只写在日志文案里。
 * 同理用 #ifndef 守卫：将来 SDK 补齐后会自动改走官方宏，不会重复定义。
 *
 * 依赖说明：本块刻意**不新增任何 #include**。宏体在**调用点**才展开，届时
 *   IAudioClient（<audioclient.h>，已含 CINTERFACE，见上方包含区）/ HRESULT /
 *   STDMETHODCALLTYPE（<objbase.h> / <windows.h>）均已定义完毕，可直接展开为合法表达式。
 * ------------------------------------------------------------------------- */
#ifndef IAudioClient_GetCurrentState
#  define IAudioClient_GetCurrentState(This, pState)                                        \
    ((HRESULT (STDMETHODCALLTYPE*)(IAudioClient*, int*))((void**)(This)->lpVtbl)[15])((This), (pState))
#endif

/* ---------------------------------------------------------------------
 * 模块内常量（零默认值原则：这些是「实现细节参数」，不是调用方的默认值）
 * ------------------------------------------------------------------- */
#define AUDIOCAP_POLL_SLEEP_MS           10      /* 环回模式不能用事件回调，采集线程轮询间隔 */
#define AUDIOCAP_STREAM_INIT_TIMEOUT_MS  3000    /* start() 等待采集线程「管线就绪」的上限 */
#define AUDIOCAP_STREAM_STOP_TIMEOUT_MS  3000    /* stop() 等待采集线程自然退出的上限 */
/* 请求 WASAPI 的引擎缓冲时长；配合 AUTOCONVERTPCM 让系统把采集格式转成目标格式。
 * 单位 100ns；类型用 LONGLONG（REFERENCE_TIME 的底层类型）以免依赖 mmsystem.h。 */
#define AUDIOCAP_BUFFER_DURATION_100NS   ((LONGLONG)1000000)  /* 100 ms */
#define AUDIOCAP_MAX_SRC_CHANNELS        32      /* 单帧最大源声道数（超出部分丢弃并 warn） */

/* ---- 可观测性节流参数（本轮新增）----
 * 下列「观测日志」都是**周期 / 条件触发**，绝不允许随采集循环（10 ms 一轮）刷屏，
 * 故统一按最小间隔节流。取值理由：
 *   - 2 s：既能反映「有没有在进数据」，又不会在长时间运行时淹没日志；
 *   - 3 s：略大于 WASAPI 引擎缓冲时长（100 ms）的 30 倍，足以滤掉偶发空转，
 *           又能让「端点挂起」在现场几秒内就被看见。 */
#define AUDIOCAP_LOG_INTERVAL_MS   2000   /* 观测日志的最小间隔（毫秒），避免刷屏 */
#define AUDIOCAP_IDLE_WARN_MS      3000   /* 连续「零包」达到该时长即告警（端点可能挂起） */

/* lastError 文本缓冲容量（契约：NUL 结尾、绝不返回 NULL、调用方只读）。
 * 容量参照 platform_websocket.c 的 PLATFORM_WS_ERR_BUF_SIZE。 */
#define AUDIOCAP_ERR_BUF_SIZE      512

/* 目标格式（WAVEFORMATEXTENSIBLE.SubFormat）用的两个标准子格式 GUID。
 * 刻意手写常量而非依赖 <ksmedia.h> 的 DEFINE_GUID（后者在未定义 INITGUID 时
 * 只做声明，且会引入大量无关符号）；值即 KSDATAFORMAT_SUBTYPE_PCM /
 * KSDATAFORMAT_SUBTYPE_IEEE_FLOAT。 */
static const GUID audioCapSubtypePcm =
    { 0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
static const GUID audioCapSubtypeIeeeFloat =
    { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };

/* 具体实现实例（刻意定义在此文件作用域内，绝不进头文件，理由见文件顶部说明）。 */
typedef struct PlatformAudioCaptureWindows{
    PlatformAudioCaptureConfig config;    /* 配置副本（deviceId 已深拷贝进 deviceId[]） */
    char                       deviceId[PLATFORM_AUDIO_DEVICE_ID_CAP];
    PlatformAudioCaptureFormat format;    /* 实际生效格式（含实际采样率，setSampleRate 会改） */

    unsigned char* ring;                  /* 环形缓冲（构造时一次性分配，运行期零 malloc） */
    int            ringCap;
    int            ringWrite;
    int            ringRead;
    int            ringUsed;
    int            overflowWarned;        /* 溢出告警去抖：只报一次 */

    PlatformMutex* mutex;                 /* 保护环形缓冲（单生产者 / 单消费者） */

    ThreadObject*  thread;

    volatile int   isQuit;                /* 请求采集线程退出 */
    volatile int   isRunning;             /* 采集线程已进入采集主循环 */
    volatile int   isPaused;              /* 暂停：线程存活但不写入环形缓冲 */
    volatile int   pipelineState;         /* 0 = 未就绪；1 = 管线就绪；-1 = 创建失败 */
    volatile int   captureError;          /* <0 = 采集出错（供 read 上报） */

    int            started;               /* 逻辑启动状态（幂等判定用） */
    int            lastError;             /* 最近一次失败的 HRESULT / 错误码（诊断用） */
    char           lastErrorText[AUDIOCAP_ERR_BUF_SIZE]; /* lastError() 返回的只读缓冲 */
                                                  /* （契约：NUL 结尾、绝不返回 NULL、
                                                  *   调用方只读、不得 free） */

    /* ---- 可观测性统计（本轮新增；构造期显式置 0，start() 时复位）----
     * 说明：这些成员**只被采集线程写**（单写者，无需加锁）；环形水位单独取锁读取，
     * 故不与 ring* 共享一把锁，避免给热路径（audioCapRingPush）增加竞争。
     * stat* 为**开环累计**（stop() 刻意不清，保留为诊断信息）；
     * statDelta* 为「距上次打日志」的增量，每次打点后归零。 */
    unsigned long long statBytes;          /* 累计写入环形缓冲的字节数（不清零） */
    unsigned long      statPackets;        /* 累计成功处理的包数（不清零） */
    unsigned long      statSilentPackets;  /* 累计静音包数（不清零，AUDCLNT_BUFFERFLAGS_SILENT） */
    unsigned long long statDeltaBytes;     /* 本周期写入字节数（打点后清零） */
    unsigned long      statDeltaPackets;   /* 本周期包数（打点后清零） */
    unsigned long      statDeltaSilentPackets; /* 本周期静音包数（打点后清零） */
    unsigned long      statLastMs;         /* 上次打「写入统计」的时刻（节流） */
    unsigned long      statIdleLastMs;     /* 上次打「零包 / 端点状态」告警的时刻（节流） */
    unsigned long      lastReadLogMs;      /* 上次打「read 交不出货」的时刻（节流） */
    unsigned long      lastInvalidCfgMs;   /* 上次打「转码器配置非法」的时刻（节流） */
    unsigned long      lastFullWarnMs;     /* 上次打「输出缓冲写满」的时刻（节流） */
    unsigned long      lastDropWarnMs;     /* 上次打「frames>0 但 0 字节产出」的时刻（节流） */
} PlatformAudioCaptureWindows;

/* ---------------------------------------------------------------------
 * 环形缓冲（单生产者：采集线程；单消费者：read / availableBytes / clear /
 * setSampleRate）。全部操作在 mutex 保护下进行；写入溢出则**覆盖最旧数据**。
 * 不变量：0 <= ringUsed <= ringCap；ringUsed == ringCap 时 ringWrite == ringRead。
 * ------------------------------------------------------------------- */
static void audioCapLock(PlatformAudioCaptureWindows* self)
{
    if (self->mutex != NULL && self->mutex->interfaces.lock != NULL) {
        self->mutex->interfaces.lock(self->mutex->instance);
    }
}

static void audioCapUnlock(PlatformAudioCaptureWindows* self)
{
    if (self->mutex != NULL && self->mutex->interfaces.unlock != NULL) {
        self->mutex->interfaces.unlock(self->mutex->instance);
    }
}

/* ---------------------------------------------------------------------
 * 可观测性与诊断辅助（本轮新增；纯诊断，**不参与任何控制流判断**）
 * ------------------------------------------------------------------- */

/* 取当前毫秒时钟。跟随本模块既有时间源（audioCapDoStart / audioCapDoStop 用的
 * PlatformTimestamp.milliseconds()，Windows 上即 GetTickCount 语义），不引入新依赖。
 * 截断到 unsigned long 是刻意的：各节流时刻同宽，差值用无符号回绕算（见
 * audioCapThrottleDue），故 ~49.7 天的回绕对本模块的 2~3 秒节流窗口无影响。 */
static unsigned long audioCapNowMs(void)
{
    PlatformTimestampInterface ts = NewPlatformTimestamp();

    if (ts.milliseconds == NULL) {
        return 0;
    }
    return (unsigned long)ts.milliseconds();
}

/* 节流闸门：距上次放行（*lastMs）已达 intervalMs 则放行并刷新 *lastMs，返回 1；
 * 否则返回 0。差值用无符号减法，天然容忍时钟回绕。
 * 注意：*lastMs 初值为 0 时 nowMs 通常远大于 intervalMs，故**首条必定放行**
 * （这正是期望行为：首次异常要立刻可见，之后才节流）。 */
static int audioCapThrottleDue(unsigned long* lastMs, unsigned long intervalMs, unsigned long nowMs)
{
    if (lastMs == NULL) {
        return 0;
    }
    if ((unsigned long)(nowMs - *lastMs) < intervalMs) {
        return 0;
    }
    *lastMs = nowMs;
    return 1;
}

/* 把失败原因写入实例内部的只读文本缓冲（lastError() 的返回对象）。
 * 与 platform_websocket.c 的 wsSetErrorf 同款：**只写缓冲、不打日志**，
 * 避免与既有 log_error 重复刷屏；截断是安全的（纯人读诊断，不参与逻辑判断）。 */
static void audioCapSetErrorf(PlatformAudioCaptureWindows* self, const char* fmt, ...)
{
    va_list ap;

    if (self == NULL || fmt == NULL) {
        return;
    }

    va_start(ap, fmt);
    vsnprintf(self->lastErrorText, sizeof(self->lastErrorText), fmt, ap);
    va_end(ap);
    self->lastErrorText[sizeof(self->lastErrorText) - 1] = '\0';
}

/* 写入 len 字节；溢出时覆盖最旧数据 */
static void audioCapRingPush(PlatformAudioCaptureWindows* self, const unsigned char* data, int len)
{
    int overflow = 0;

    if (self == NULL || data == NULL || len <= 0 || self->ring == NULL || self->ringCap <= 0) {
        return;
    }

    audioCapLock(self);

    if (len >= self->ringCap) {
        /* 单次写入即超过容量：只保留最后 ringCap 字节 */
        memcpy(self->ring, data + (len - self->ringCap), (size_t)self->ringCap);
        self->ringWrite = 0;
        self->ringRead  = 0;
        self->ringUsed  = self->ringCap;
        overflow = 1;
    } else {
        int first;

        if (self->ringUsed + len > self->ringCap) {
            int drop = self->ringUsed + len - self->ringCap;
            self->ringRead = (self->ringRead + drop) % self->ringCap;
            self->ringUsed -= drop;
            overflow = 1;
        }

        first = self->ringCap - self->ringWrite;
        if (first > len) {
            first = len;
        }
        memcpy(self->ring + self->ringWrite, data, (size_t)first);
        if (len - first > 0) {
            memcpy(self->ring, data + first, (size_t)(len - first));
        }
        self->ringWrite = (self->ringWrite + len) % self->ringCap;
        self->ringUsed += len;
    }

    audioCapUnlock(self);

    if (overflow && !self->overflowWarned) {
        self->overflowWarned = 1;
        log_warn("platform_audio_capture(WASAPI): ring buffer overflow, oldest data overwritten "
                 "(further overflow warnings suppressed)");
    }
}

/* 记一笔「本次处理了一个包」。由 audioCapHandlePacket 在**确认写入环形缓冲后**调用，
 * 故 statBytes 恒等于「真正进了环形缓冲的字节数」（而不是源端收到的字节数）。
 * 只被采集线程调用（单写者），无需加锁。 */
static void audioCapNoteWrite(PlatformAudioCaptureWindows* self, int bytes, int silent)
{
    if (self == NULL) {
        return;
    }
    if (bytes > 0) {
        self->statBytes      += (unsigned long long)bytes;
        self->statDeltaBytes += (unsigned long long)bytes;
    }
    self->statPackets++;
    self->statDeltaPackets++;
    if (silent) {
        self->statSilentPackets++;
        self->statDeltaSilentPackets++;
    }
}

/* 周期「写入统计」日志（节流 AUDIOCAP_LOG_INTERVAL_MS）。
 * 同时打「本周期增量」与「开环累计」：增量回答「现在还在不在进数据」，
 * 累计回答「整个 run 一共进了多少」。环形水位另取一次锁（快照），不持锁做日志 I/O。 */
static void audioCapWriteStatus(PlatformAudioCaptureWindows* self, unsigned long nowMs)
{
    unsigned long      prevMs;
    unsigned long      elapsedMs;
    unsigned long long totalBytes;
    unsigned long      totalPackets;
    unsigned long      totalSilent;
    int                ringUsed;
    int                ringCap;

    if (self == NULL) {
        return;
    }

    /* 先留一份「上次打点」的时刻：audioCapThrottleDue 内部会就地刷新 statLastMs，
     * 若在其后才算差值，elapsedMs 恒为 0（节流失效）。 */
    prevMs = self->statLastMs;
    if (!audioCapThrottleDue(&self->statLastMs, AUDIOCAP_LOG_INTERVAL_MS, nowMs)) {
        return;
    }
    elapsedMs = (unsigned long)(nowMs - prevMs);

    audioCapLock(self);
    ringUsed = self->ringUsed;
    ringCap  = self->ringCap;
    audioCapUnlock(self);

    totalBytes   = self->statBytes;          /* 累计值：保留不清，跨 stop() 保留为诊断信息 */
    totalPackets = self->statPackets;
    totalSilent  = self->statSilentPackets;

    log_info("audioCapWriteStatus: +%llu bytes in %lu ms (packets=%lu, silent=%lu, ring=%d/%d), "
             "cumulative=%llu bytes/%lu packets (silent=%lu)",
             self->statDeltaBytes, elapsedMs, self->statDeltaPackets, self->statDeltaSilentPackets,
             ringUsed, ringCap, totalBytes, totalPackets, totalSilent);

    /* 只清 delta，累计值保留 */
    self->statDeltaBytes         = 0;
    self->statDeltaPackets       = 0;
    self->statDeltaSilentPackets = 0;
}

/* ---------------------------------------------------------------------
 * 内部转码器：源设备格式 -> 目标交付格式
 *   位深/类型：float32 / int16 / int24(容器) / int32 -> int16 或 int32（钳位）
 *   声道：源 N 声道 -> 目标 1（各声道平均）或 2（取前两声道；源单声道则复制）
 *   重采样：**零阶保持（zero-order hold / 最近邻取样）**。
 *     ⚠️ 准确的实现描述（**本轮只纠正注释，算法未动**）：
 *        循环条件为 pos < g，其中 g = prevGlobalIndex + 1 恒为整数，而
 *        pos = outCount * step。当 step 为**整数**（即整数倍降采样，最常见的是
 *        48 kHz -> 16 kHz，step = 3.0）时 pos 恒为整数且 pos <= prevGlobalIndex，
 *        故相位分数 f = pos - prevGlobalIndex 恒 <= 0，被钳到 0 ⇒
 *        产出的目标帧**就是上一个源帧的值**，即严格的**零阶保持**。
 *        只有 step 为非整数（升采样、或 44.1k->16k 这类非整数比降采样）时
 *        pos 才会带小数部分，f 才可能取到 (0,1) 的插值权重 —— 即那种情形下
 *        才发生「线性插值」，但权重仍被钳在 [0,1] 内、且逐帧跳变不连续。
 *     质量取舍：零阶保持实现简单、延迟低，但**带通降采样存在混叠（aliasing）风险**
 *       （48k->16k 未加抗混叠低通，高频会折叠到可听频带内）。若需要高质量重采样，
 *       应改用带多相滤波 / 带限 sinc 的重采样器 —— 这需要上层或后续改造
 *       （本轮**未获授权**改算法，故仅在此把注释改成与实现相符的描述）。
 * 说明：跨包连续性由 prevMixed（上一个源帧的「已混音」值）+ 全局输出计数保证。
 * ------------------------------------------------------------------- */
typedef struct AudioCapConverter{
    int       srcRate;
    int       srcChannels;
    int       srcBits;         /* 源「容器」位深（用于步进）：8 / 16 / 24 / 32 */
    int       srcBytesPerSample;
    int       srcIsFloat;
    int       srcBlockAlign;   /* 源帧字节数 */

    int       dstRate;
    int       dstChannels;
    int       dstBits;
    int       dstBytesPerSample;
    int       dstBlockAlign;   /* 目标帧字节数 */

    double    step;            /* srcRate / dstRate：每输出一个目标帧前进的源帧数 */
    long long outCount;        /* 已产出的目标帧总数（单调递增，用于定位源位置） */
    long long prevGlobalIndex; /* prevMixed 对应的源帧全局序号；-1 = 尚未就绪 */
    double    prevMixed[2];    /* 上一个源帧的已混音值（目标声道数 <= 2） */

    /* ---- 诊断（本轮新增）---- */
    PlatformAudioCaptureWindows* owner;  /* 宿主实例：仅用于把失败原因写进 lastError
                                          * 文本缓冲（可为 NULL，之后所有诊断自动降级为
                                          * 「只打日志、不写 lastError」）。刻意不传 self
                                          * 作形参，避免改动既有函数签名。 */
} AudioCapConverter;

static void audioCapConverterReset(AudioCapConverter* cv)
{
    memset(cv, 0, sizeof(*cv));
    cv->prevGlobalIndex = -1;
    cv->outCount        = 0;
    cv->owner           = NULL;   /* 宿主由 audioCapPipelineOpen 在配置转码器时回填 */
}

/* 解析一个源采样为归一化 double（[-1,1] 附近，可越界，由编码端钳位） */
static double audioCapDecodeSample(const unsigned char* p, int bits, int isFloat)
{
    if (isFloat) {
        float f;
        memcpy(&f, p, sizeof(f));
        return (double)f;
    }

    switch (bits) {
    case 8: {  /* 8 位 PCM 为无符号 */
        return ((double)p[0] - 128.0) / 128.0;
    }
    case 16: {
        short s;
        memcpy(&s, p, 2);
        return (double)s / 32768.0;
    }
    case 24: {
        unsigned int u = (unsigned int)p[0] | ((unsigned int)p[1] << 8) | ((unsigned int)p[2] << 16);
        int v = (int)u;
        if (v & 0x00800000) {
            v |= (int)0xFF000000;   /* 符号扩展 */
        }
        return (double)v / 8388608.0;
    }
    case 32: {
        int v;
        memcpy(&v, p, 4);
        return (double)v / 2147483648.0;
    }
    default:
        return 0.0;
    }
}

static long audioCapRoundToLong(double v)
{
    return (long)(v >= 0.0 ? (v + 0.5) : (v - 0.5));
}

/* 把一个归一化 double 编码为目标位深（小端、PCM 有符号） */
static void audioCapEncodeSample(unsigned char* p, int bits, double v)
{
    if (v > 1.0) {
        v = 1.0;
    } else if (v < -1.0) {
        v = -1.0;
    }

    if (bits == 16) {
        long s = audioCapRoundToLong(v * 32767.0);
        if (s > 32767)  { s = 32767; }
        if (s < -32768) { s = -32768; }
        p[0] = (unsigned char)((unsigned long)s & 0xFFu);
        p[1] = (unsigned char)(((unsigned long)s >> 8) & 0xFFu);
    } else { /* bits == 32 */
        double scaled = v * 2147483647.0;
        long long s;
        if (scaled > 2147483647.0)   { scaled = 2147483647.0; }
        if (scaled < -2147483648.0)  { scaled = -2147483648.0; }
        s = (long long)audioCapRoundToLong(scaled);
        p[0] = (unsigned char)((unsigned long long)s & 0xFFull);
        p[1] = (unsigned char)(((unsigned long long)s >> 8) & 0xFFull);
        p[2] = (unsigned char)(((unsigned long long)s >> 16) & 0xFFull);
        p[3] = (unsigned char)(((unsigned long long)s >> 24) & 0xFFull);
    }
}

/* 把一个源帧解码并混音为目标声道数（写入 outMixed[0..dstChannels-1]） */
static void audioCapMixFrame(const AudioCapConverter* cv, const unsigned char* frame, double* outMixed)
{
    double chans[AUDIOCAP_MAX_SRC_CHANNELS];
    int    c;

    for (c = 0; c < cv->srcChannels && c < AUDIOCAP_MAX_SRC_CHANNELS; c++) {
        chans[c] = audioCapDecodeSample(frame + (size_t)c * (size_t)cv->srcBytesPerSample,
                                        cv->srcBits, cv->srcIsFloat);
    }

    if (cv->dstChannels == 1) {
        double sum = 0.0;
        int    n   = (cv->srcChannels < AUDIOCAP_MAX_SRC_CHANNELS) ? cv->srcChannels
                                                                   : AUDIOCAP_MAX_SRC_CHANNELS;
        for (c = 0; c < n; c++) {
            sum += chans[c];
        }
        outMixed[0] = (n > 0) ? (sum / (double)n) : 0.0;
    } else if (cv->srcChannels == 1) {
        outMixed[0] = chans[0];
        outMixed[1] = chans[0];
    } else {
        outMixed[0] = chans[0];
        outMixed[1] = chans[1];
    }
}

/* 转码：src 共 srcBytes 字节 -> dst（容量 dstCapBytes），返回写入字节数 */
static int audioCapConvert(AudioCapConverter* cv, const unsigned char* src, int srcBytes,
                           unsigned char* dst, int dstCapBytes)
{
    int       frames;
    int       outBytes = 0;
    int       i;
    long long g;

    if (cv->srcBlockAlign <= 0 || cv->dstBlockAlign <= 0 || cv->step <= 0.0) {
        /* 【真异常】转码器配置非法：本次调用必然交出 0 字节（原先**完全静默**，
         * 现场只能表现为「一直读不到数据」）。本轮只补观测：**仍然 return 0**，
         * 不改成 -1，以避免改变任何既有行为。 */
        PlatformAudioCaptureWindows* owner = cv->owner;
        unsigned long               nowMs = audioCapNowMs();

        if (owner != NULL) {
            audioCapSetErrorf(owner,
                "audioCapConvert: invalid converter config "
                "(srcBlockAlign=%d dstBlockAlign=%d step=%f) -> 0 bytes",
                cv->srcBlockAlign, cv->dstBlockAlign, cv->step);
        }
        if (owner != NULL &&
            audioCapThrottleDue(&owner->lastInvalidCfgMs, AUDIOCAP_LOG_INTERVAL_MS, nowMs)) {
            log_error("audioCapConvert: invalid converter config "
                      "(srcBlockAlign=%d dstBlockAlign=%d step=%f) -> 0 bytes",
                      cv->srcBlockAlign, cv->dstBlockAlign, cv->step);
        }
        return 0;
    }

    frames = srcBytes / cv->srcBlockAlign;

    for (i = 0; i < frames; i++) {
        const unsigned char* frame = src + (size_t)i * (size_t)cv->srcBlockAlign;
        double mixed[2];

        audioCapMixFrame(cv, frame, mixed);
        g = (cv->prevGlobalIndex < 0) ? 0 : (cv->prevGlobalIndex + 1);

        if (cv->prevGlobalIndex < 0) {
            /* 首个源帧：只作为「保持值」的初始状态，不产出（无前序帧可保持） */
            cv->prevMixed[0] = mixed[0];
            cv->prevMixed[1] = (cv->dstChannels > 1) ? mixed[1] : mixed[0];
            cv->prevGlobalIndex = g;
            continue;
        }

        /* 产出所有落在 [prevGlobalIndex, g] 区间内的目标帧 */
        for (;;) {
            double pos = (double)cv->outCount * cv->step;
            double f;
            int    c;

            if (pos >= (double)g) {
                break;
            }
            /* 相位分数 f = 目标帧在源帧区间内的相对位置 ∈ [0,1]。
             * ⚠️ 整数倍降采样时（如 48 kHz -> 16 kHz，step = 3.0）f **恒为 0**，
             *    故下方 v 实际就等于 prevMixed —— 即**零阶保持**（未做插值）。 */
            f = pos - (double)cv->prevGlobalIndex;
            if (f < 0.0) {
                f = 0.0;
            }
            if (f > 1.0) {
                f = 1.0;
            }

            if (outBytes + cv->dstBlockAlign > dstCapBytes) {
                /* 输出缓冲不足（容量已按引擎缓冲上限算足，正常不会触发）：
                 * 本包剩余的目标帧在此被**截断丢弃**，原先无任何痕迹。
                 * 本轮只补节流告警；**返回 outBytes 的行为不变**。 */
                PlatformAudioCaptureWindows* owner = cv->owner;

                if (owner != NULL) {
                    audioCapSetErrorf(owner,
                        "audioCapConvert: output buffer full, at least %d bytes dropped "
                        "(srcRate=%d dstRate=%d)",
                        cv->dstBlockAlign, cv->srcRate, cv->dstRate);
                    if (audioCapThrottleDue(&owner->lastFullWarnMs,
                                             AUDIOCAP_LOG_INTERVAL_MS, audioCapNowMs())) {
                        /* 丢掉的字节数是「下界」：至少一帧（dstBlockAlign）写不下，
                         * 其余剩余帧因提前 return 而无法精确统计。 */
                        log_warn("audioCapConvert: output buffer full, %d bytes dropped "
                                 "(srcRate=%d dstRate=%d)",
                                 cv->dstBlockAlign, cv->srcRate, cv->dstRate);
                    }
                }
                cv->prevMixed[0] = mixed[0];
                cv->prevMixed[1] = (cv->dstChannels > 1) ? mixed[1] : mixed[0];
                cv->prevGlobalIndex = g;
                return outBytes;
            }

            for (c = 0; c < cv->dstChannels; c++) {
                double v = cv->prevMixed[c] * (1.0 - f) + mixed[c] * f;
                audioCapEncodeSample(dst + outBytes + (size_t)c * (size_t)cv->dstBytesPerSample,
                                     cv->dstBits, v);
            }
            outBytes += cv->dstBlockAlign;
            cv->outCount++;
        }

        cv->prevMixed[0] = mixed[0];
        cv->prevMixed[1] = (cv->dstChannels > 1) ? mixed[1] : mixed[0];
        cv->prevGlobalIndex = g;
    }

    return outBytes;
}

/* ---------------------------------------------------------------------
 * COM 初始化：采集线程内自行 CoInitialize（线程由 ThreadObject 创建），
 * 线程退出前配对 CoUninitialize。返回 0 = 可用；<0 = 失败。
 * needUninit 回传「本线程是否需要在退出时 CoUninitialize」。
 * ------------------------------------------------------------------- */
static int audioCapComInit(int* needUninit)
{
    HRESULT hr;

    *needUninit = 0;

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (hr == S_OK || hr == S_FALSE) {
        /* S_OK：首次初始化；S_FALSE：同模型重复初始化。两者都增加了引用计数，需配对释放。 */
        *needUninit = 1;
        return 0;
    }
    if (hr == RPC_E_CHANGED_MODE) {
        /* 本线程已以其它单元模型初始化过 COM：接口依旧可用，只是不要 CoUninitialize。 */
        *needUninit = 0;
        return 0;
    }

    return -1;
}

/* 宽字符 -> UTF-8（设备 id / 友好名均为 ASCII 兼容，UTF-8 足够且稳定）。失败时置空串。 */
static void audioCapWideToUtf8(const WCHAR* wide, char* out, int outCap)
{
    int n;

    if (out == NULL || outCap <= 0) {
        return;
    }
    out[0] = '\0';
    if (wide == NULL) {
        return;
    }

    n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, outCap, NULL, NULL);
    if (n <= 0) {
        out[0] = '\0';
    }
    out[outCap - 1] = '\0';
}

/* 由目标交付格式构造 WAVEFORMATEXTENSIBLE（PCM 子格式，左对齐有效位） */
static void audioCapBuildTargetFormat(const PlatformAudioCaptureFormat* fmt, WAVEFORMATEXTENSIBLE* out)
{
    memset(out, 0, sizeof(*out));

    out->Format.wFormatTag      = WAVE_FORMAT_EXTENSIBLE;
    out->Format.nChannels       = (WORD)fmt->channels;
    out->Format.nSamplesPerSec  = (DWORD)fmt->sampleRate;
    out->Format.wBitsPerSample  = (WORD)fmt->bitsPerSample;
    out->Format.nBlockAlign     = (WORD)((fmt->channels * fmt->bitsPerSample) / 8);
    out->Format.nAvgBytesPerSec = (DWORD)(fmt->sampleRate * (int)out->Format.nBlockAlign);
    out->Format.cbSize          = (WORD)(sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX));

    out->Samples.wValidBitsPerSample = (WORD)fmt->bitsPerSample;
    out->dwChannelMask = (fmt->channels == 2)
        ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT)
        : SPEAKER_FRONT_CENTER;
    out->SubFormat = audioCapSubtypePcm;
}

/* 【注意 · 定义顺序】AudioCapPipeline 必须**先于** audioCapParseSourceFormat 定义：
 * 后者形参为 AudioCapPipeline*，且函数体内解引用其成员，C 要求此处为**完整类型**
 * （仅加前向声明会得到一批 "invalid use of incomplete typedef" 错误）。
 * 故本 typedef 由此处的原位置整体上移（内容逐字未改）。 */
/* ---------------------------------------------------------------------
 * 采集管线：COM 对象 + 转码所需缓冲 / 状态。生命周期 == 采集线程内的
 * 「打开 -> 主循环 -> 关闭」，全部资源只在本线程创建与释放。
 * ------------------------------------------------------------------- */
typedef struct AudioCapPipeline{
    IMMDeviceEnumerator* enumerator;
    IMMDevice*           device;
    IAudioClient*        audioClient;
    IAudioCaptureClient* captureClient;

    int                  comInited;      /* 本线程是否需 CoUninitialize */
    int                  started;        /* IAudioClient_Start 是否已调用 */
    int                  useAutoConvert; /* 1 = WASAPI(AUTOCONVERTPCM) 转码 */
    int                  isPassthrough;  /* 源格式 == 目标格式（直接拷贝） */

    int                  srcRate;
    int                  srcChannels;
    int                  srcBits;
    int                  srcBytesPerSample;
    int                  srcIsFloat;
    int                  srcBlockAlign;

    unsigned char*       srcScratch;     /* 源数据暂存 / 静音零缓冲 */
    int                  srcScratchBytes;
    unsigned char*       dstScratch;     /* 转码输出缓冲 */
    int                  dstScratchBytes;

    AudioCapConverter    converter;
} AudioCapPipeline;

/* 解析源（采集端）实际格式，填入管线。注意：WAVEFORMATEXTENSIBLE 下 PCM 采样
 * 为**左对齐**（有效位在高位），故统一按容器位深 wBitsPerSample 归一化即可。 */
static void audioCapParseSourceFormat(const WAVEFORMATEX* wfx, AudioCapPipeline* pipe)
{
    pipe->srcRate           = (int)wfx->nSamplesPerSec;
    pipe->srcChannels       = (int)wfx->nChannels;
    pipe->srcBits           = (int)wfx->wBitsPerSample;
    pipe->srcBlockAlign     = (int)wfx->nBlockAlign;
    pipe->srcBytesPerSample = pipe->srcBits / 8;
    pipe->srcIsFloat        = 0;

    if (pipe->srcBytesPerSample <= 0) {
        pipe->srcBytesPerSample = 1;
    }

    if (wfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        pipe->srcIsFloat = 1;
    } else if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const WAVEFORMATEXTENSIBLE* ext = (const WAVEFORMATEXTENSIBLE*)wfx;
        if (IsEqualGUID(&ext->SubFormat, &audioCapSubtypeIeeeFloat)) {
            pipe->srcIsFloat = 1;
        }
    }
}

/* 释放采集管线（幂等；可安全用于「部分创建」的管线） */
static void audioCapPipelineClose(AudioCapPipeline* pipe)
{
    if (pipe == NULL) {
        return;
    }

    if (pipe->audioClient != NULL && pipe->started) {
        IAudioClient_Stop(pipe->audioClient);
        pipe->started = 0;
    }
    if (pipe->captureClient != NULL) {
        IAudioCaptureClient_Release(pipe->captureClient);
        pipe->captureClient = NULL;
    }
    if (pipe->audioClient != NULL) {
        IAudioClient_Release(pipe->audioClient);
        pipe->audioClient = NULL;
    }
    if (pipe->device != NULL) {
        IMMDevice_Release(pipe->device);
        pipe->device = NULL;
    }
    if (pipe->enumerator != NULL) {
        IMMDeviceEnumerator_Release(pipe->enumerator);
        pipe->enumerator = NULL;
    }
    if (pipe->srcScratch != NULL) {
        audioCaptureFree(pipe->srcScratch);
        pipe->srcScratch = NULL;
    }
    if (pipe->dstScratch != NULL) {
        audioCaptureFree(pipe->dstScratch);
        pipe->dstScratch = NULL;
    }

    if (pipe->comInited) {
        CoUninitialize();
        pipe->comInited = 0;
    }
}

/* 打开采集管线：选设备 -> Activate -> Initialize -> GetService -> 分配缓冲 -> Start。
 * 返回 0 = 成功；<0 = 失败（失败时已释放全部已获取资源）。 */
static int audioCapPipelineOpen(PlatformAudioCaptureWindows* self, AudioCapPipeline* pipe)
{
    EDataFlow            flow;
    HRESULT              hr;
    WAVEFORMATEXTENSIBLE target;

    memset(pipe, 0, sizeof(*pipe));
    audioCapConverterReset(&pipe->converter);

    if (audioCapComInit(&pipe->comInited) != 0) {
        audioCapSetErrorf(self, "CoInitializeEx failed (capture thread COM init)");
        log_error("platform_audio_capture(WASAPI): CoInitializeEx failed");
        return -1;
    }

    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                          &IID_IMMDeviceEnumerator, (void**)&pipe->enumerator);
    if (FAILED(hr) || pipe->enumerator == NULL) {
        audioCapSetErrorf(self, "CoCreateInstance(CLSID_MMDeviceEnumerator) failed hr=0x%08X",
                          (unsigned)hr);
        log_error("platform_audio_capture(WASAPI): create MMDeviceEnumerator failed hr=0x%08X", (unsigned)hr);
        goto fail;
    }

    flow = (self->config.sourceType == PLATFORM_AUDIO_SOURCE_SYSTEM) ? eRender : eCapture;

    /* ① 选设备：deviceId 非空则按 id 精确匹配；否则用该类型的默认设备 */
    if (self->deviceId[0] != '\0') {
        IMMDeviceCollection* collection = NULL;

        hr = IMMDeviceEnumerator_EnumAudioEndpoints(pipe->enumerator, flow, DEVICE_STATE_ACTIVE, &collection);
        if (SUCCEEDED(hr) && collection != NULL) {
            UINT count = 0;
            UINT i;

            if (SUCCEEDED(IMMDeviceCollection_GetCount(collection, &count))) {
                for (i = 0; i < count && pipe->device == NULL; i++) {
                    IMMDevice* dev = NULL;
                    LPWSTR     idStr = NULL;

                    if (FAILED(IMMDeviceCollection_Item(collection, i, &dev)) || dev == NULL) {
                        continue;
                    }
                    if (SUCCEEDED(IMMDevice_GetId(dev, &idStr)) && idStr != NULL) {
                        char idUtf8[PLATFORM_AUDIO_DEVICE_ID_CAP];
                        audioCapWideToUtf8(idStr, idUtf8, (int)sizeof(idUtf8));
                        CoTaskMemFree(idStr);
                        if (strcmp(idUtf8, self->deviceId) == 0) {
                            pipe->device = dev;
                            dev = NULL;   /* 所有权转移给 pipe */
                        }
                    }
                    if (dev != NULL) {
                        IMMDevice_Release(dev);
                    }
                }
            }
            IMMDeviceCollection_Release(collection);
        }
    }

    if (pipe->device == NULL) {
        hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(pipe->enumerator, flow, eConsole, &pipe->device);
        if (FAILED(hr) || pipe->device == NULL) {
            audioCapSetErrorf(self, "no audio device (srcType=%d id=%s) hr=0x%08X",
                              (int)self->config.sourceType,
                              (self->deviceId[0] != '\0') ? self->deviceId : "<default>", (unsigned)hr);
            log_error("platform_audio_capture(WASAPI): no device (srcType=%d id=%s) hr=0x%08X",
                      (int)self->config.sourceType,
                      (self->deviceId[0] != '\0') ? self->deviceId : "<default>", (unsigned)hr);
            goto fail;
        }
    }

    hr = IMMDevice_Activate(pipe->device, &IID_IAudioClient, CLSCTX_ALL, NULL, (void**)&pipe->audioClient);
    if (FAILED(hr) || pipe->audioClient == NULL) {
        audioCapSetErrorf(self, "IMMDevice_Activate(IID_IAudioClient) failed hr=0x%08X", (unsigned)hr);
        log_error("platform_audio_capture(WASAPI): Activate IAudioClient failed hr=0x%08X", (unsigned)hr);
        goto fail;
    }

    /* ② 先尝试让**系统**转码：请求目标格式 + AUTOCONVERTPCM + SRC_DEFAULT_QUALITY。
     *    环回模式与事件回调互斥，这里不用事件回调；LOOPBACK 仅系统源使用。 */
    {
        DWORD flags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

        if (self->config.sourceType == PLATFORM_AUDIO_SOURCE_SYSTEM) {
            flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;
        }

        audioCapBuildTargetFormat(&self->format, &target);

        hr = IAudioClient_Initialize(pipe->audioClient, AUDCLNT_SHAREMODE_SHARED, flags,
                                     AUDIOCAP_BUFFER_DURATION_100NS, 0,
                                     (const WAVEFORMATEX*)&target, NULL);

        if (SUCCEEDED(hr)) {
            pipe->useAutoConvert = 1;
            pipe->isPassthrough  = 1;   /* 系统已把采集格式转成目标格式：源 == 目标 */
            pipe->srcRate           = self->format.sampleRate;
            pipe->srcChannels       = self->format.channels;
            pipe->srcBits           = self->format.bitsPerSample;
            pipe->srcBytesPerSample = self->format.bitsPerSample / 8;
            pipe->srcBlockAlign     = self->format.channels * pipe->srcBytesPerSample;
            pipe->srcIsFloat        = 0;
            log_info("platform_audio_capture(WASAPI): 走 AUTOCONVERTPCM 路线（系统转码）"
                     " target=%dHz/%dch/%dbit",
                     self->format.sampleRate, self->format.channels, self->format.bitsPerSample);
        } else {
            /* 非致命：已回退到「内部转码」路线，故只写 lastError（不升级为 error 级），
             * 但仍要把原因留给调用方：若后续回退路线也失败，这行文本就是唯一的线索。 */
            audioCapSetErrorf(self, "IAudioClient_Initialize(AUTOCONVERTPCM) failed hr=0x%08X "
                              "(recovered: fallback to GetMixFormat + internal conversion)",
                              (unsigned)hr);
            log_info("platform_audio_capture(WASAPI): AUTOCONVERTPCM 失败 hr=0x%08X，"
                     "回退到「按 GetMixFormat 打开 + 平台层内部转码」", (unsigned)hr);
        }
    }

    /* ③ 回退路线：按设备混音格式打开，由平台层内部转码 */
    if (!pipe->useAutoConvert) {
        WAVEFORMATEX* mixFormat = NULL;
        DWORD         flags2 = (self->config.sourceType == PLATFORM_AUDIO_SOURCE_SYSTEM)
                               ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0;

        hr = IAudioClient_GetMixFormat(pipe->audioClient, &mixFormat);
        if (FAILED(hr) || mixFormat == NULL) {
            audioCapSetErrorf(self, "IAudioClient_GetMixFormat failed hr=0x%08X", (unsigned)hr);
            log_error("platform_audio_capture(WASAPI): GetMixFormat failed hr=0x%08X", (unsigned)hr);
            goto fail;
        }

        hr = IAudioClient_Initialize(pipe->audioClient, AUDCLNT_SHAREMODE_SHARED, flags2,
                                     AUDIOCAP_BUFFER_DURATION_100NS, 0, mixFormat, NULL);
        if (FAILED(hr)) {
            audioCapSetErrorf(self, "IAudioClient_Initialize(mixFormat) failed hr=0x%08X", (unsigned)hr);
            log_error("platform_audio_capture(WASAPI): Initialize(mixFormat) failed hr=0x%08X", (unsigned)hr);
            CoTaskMemFree(mixFormat);
            goto fail;
        }

        audioCapParseSourceFormat(mixFormat, pipe);
        CoTaskMemFree(mixFormat);
        mixFormat = NULL;

        pipe->isPassthrough = (pipe->srcRate == self->format.sampleRate &&
                               pipe->srcChannels == self->format.channels &&
                               pipe->srcBits == self->format.bitsPerSample &&
                               !pipe->srcIsFloat &&
                               pipe->srcBlockAlign == (self->format.channels * self->format.bitsPerSample / 8));

        log_info("platform_audio_capture(WASAPI): 走内部转码路线 src=%dHz/%dch/%dbit(float=%d) "
                 "-> dst=%dHz/%dch/%dbit passthrough=%d",
                 pipe->srcRate, pipe->srcChannels, pipe->srcBits, pipe->srcIsFloat,
                 self->format.sampleRate, self->format.channels, self->format.bitsPerSample,
                 pipe->isPassthrough);
    }

    hr = IAudioClient_GetService(pipe->audioClient, &IID_IAudioCaptureClient, (void**)&pipe->captureClient);
    if (FAILED(hr) || pipe->captureClient == NULL) {
        audioCapSetErrorf(self, "IAudioClient_GetService(IID_IAudioCaptureClient) failed hr=0x%08X",
                          (unsigned)hr);
        log_error("platform_audio_capture(WASAPI): GetService(IAudioCaptureClient) failed hr=0x%08X", (unsigned)hr);
        goto fail;
    }

    /* ④ 一次性分配转码缓冲（运行期零 malloc）：容量按引擎缓冲上限估算并留裕量 */
    {
        UINT32    engineFrames = 0;
        long long maxIn;
        long long maxOut;
        int       dstBlockAlign = self->format.channels * (self->format.bitsPerSample / 8);

        IAudioClient_GetBufferSize(pipe->audioClient, &engineFrames);
        if (engineFrames == 0) {
            engineFrames = (UINT32)(self->format.sampleRate > 0 ? self->format.sampleRate : 48000);
        }

        maxIn = (long long)engineFrames + 16;
        if (maxIn < 4096) {
            maxIn = 4096;   /* 覆盖单包可能大于 GetBufferSize 的边界情况 */
        }
        if (!pipe->isPassthrough && pipe->srcRate > 0) {
            maxOut = (long long)((maxIn * (long long)self->format.sampleRate) / (long long)pipe->srcRate) + 32;
        } else {
            maxOut = maxIn;
        }
        if (maxOut < maxIn) {
            maxOut = maxIn;
        }

        pipe->srcScratchBytes = (int)(maxIn * (long long)pipe->srcBlockAlign);
        pipe->dstScratchBytes = (int)(maxOut * (long long)dstBlockAlign);
        if (pipe->srcScratchBytes <= 0) { pipe->srcScratchBytes = pipe->srcBlockAlign; }
        if (pipe->dstScratchBytes <= 0) { pipe->dstScratchBytes = dstBlockAlign; }

        pipe->srcScratch = (unsigned char*)audioCaptureMalloc((size_t)pipe->srcScratchBytes);
        pipe->dstScratch = (unsigned char*)audioCaptureMalloc((size_t)pipe->dstScratchBytes);
        if (pipe->srcScratch == NULL || pipe->dstScratch == NULL) {
            audioCapSetErrorf(self, "alloc transcode scratch failed (src=%d dst=%d bytes)",
                              pipe->srcScratchBytes, pipe->dstScratchBytes);
            log_error("platform_audio_capture(WASAPI): alloc scratch failed (src=%d dst=%d bytes)",
                      pipe->srcScratchBytes, pipe->dstScratchBytes);
            goto fail;
        }

        /* 配置转码器（passthrough 时不启用，但状态仍就绪） */
        pipe->converter.owner           = self;   /* 诊断回填：供 audioCapConvert 写 lastError */
        pipe->converter.srcRate        = pipe->srcRate;
        pipe->converter.srcChannels    = pipe->srcChannels;
        pipe->converter.srcBits        = pipe->srcBits;
        pipe->converter.srcBytesPerSample = pipe->srcBytesPerSample;
        pipe->converter.srcIsFloat     = pipe->srcIsFloat;
        pipe->converter.srcBlockAlign  = pipe->srcBlockAlign;
        pipe->converter.dstRate        = self->format.sampleRate;
        pipe->converter.dstChannels    = self->format.channels;
        pipe->converter.dstBits        = self->format.bitsPerSample;
        pipe->converter.dstBytesPerSample = self->format.bitsPerSample / 8;
        pipe->converter.dstBlockAlign  = dstBlockAlign;
        pipe->converter.step = (pipe->srcRate > 0)
            ? ((double)pipe->srcRate / (double)self->format.sampleRate) : 1.0;
    }

    hr = IAudioClient_Start(pipe->audioClient);
    if (FAILED(hr)) {
        audioCapSetErrorf(self, "IAudioClient_Start failed hr=0x%08X", (unsigned)hr);
        log_error("platform_audio_capture(WASAPI): IAudioClient_Start failed hr=0x%08X", (unsigned)hr);
        goto fail;
    }
    pipe->started = 1;

    return 0;

fail:
    audioCapPipelineClose(pipe);
    return -1;
}

/* ---------------------------------------------------------------------
 * 把一个采集包写入环形缓冲（passthrough 直接拷贝；否则走内部转码）
 * ------------------------------------------------------------------- */
static void audioCapHandlePacket(PlatformAudioCaptureWindows* self, AudioCapPipeline* pipe,
                                 const BYTE* data, UINT32 frames, int silent)
{
    if (frames == 0 || pipe->srcBlockAlign <= 0) {
        /* **正常情况，保持静默（刻意不打日志）**：
         *   - frames == 0：GetBuffer 刚取到缓冲但本轮无数据，属预期内，不该报警；
         *   - srcBlockAlign <= 0：管线配置阶段的异常，但同一处已由
         *     audioCapPipelineOpen 的 log_error 覆盖，此处再打只会重复刷屏。
         * 真正需要暴露的是「有帧却转不出字节」，见本函数末尾的节流告警。 */
        return;
    }

    if (pipe->isPassthrough) {
        int bytes = (int)frames * pipe->srcBlockAlign;
        int pushed = 0;   /* 真正写进环形缓冲的字节数（0 = 本包无处可写） */

        if (silent) {
            if (bytes > pipe->dstScratchBytes) {
                bytes = pipe->dstScratchBytes;
            }
            memset(pipe->dstScratch, 0, (size_t)bytes);
            audioCapRingPush(self, pipe->dstScratch, bytes);
            pushed = bytes;
        } else if (data != NULL) {
            audioCapRingPush(self, data, bytes);
            pushed = bytes;
        } else {
            /* **正常情况，保持静默**：非静音包但 data 为 NULL（WASAPI 给了空数据指针，
             * 极少见）。无字节可写，也不计入统计，避免污染「写入统计」口径。 */
        }
        /* passthrough 下 bytes >= 1，故只要 pushed > 0 就不存在「0 字节产出」，无需告警 */
        if (pushed > 0) {
            audioCapNoteWrite(self, pushed, silent);
        }
        return;
    }

    {
        int srcBytes = (int)frames * pipe->srcBlockAlign;

        if (srcBytes > pipe->srcScratchBytes) {
            srcBytes = pipe->srcScratchBytes;
        }
        if (silent) {
            /* 静音包：用零填充源缓冲，保证转码器全局帧序号与相位连续 */
            memset(pipe->srcScratch, 0, (size_t)srcBytes);
        } else {
            if (data == NULL) {
                /* **正常情况，保持静默**：同 passthrough 分支 —— 非静音包却拿到 NULL
                 * 数据指针，属 WASAPI 侧的边角情况；无字节可写，直接返回。 */
                return;
            }
            memcpy(pipe->srcScratch, data, (size_t)srcBytes);
        }

        {
            int outBytes = audioCapConvert(&pipe->converter, pipe->srcScratch, srcBytes,
                                           pipe->dstScratch, pipe->dstScratchBytes);
            if (outBytes > 0) {
                audioCapRingPush(self, pipe->dstScratch, outBytes);
                audioCapNoteWrite(self, outBytes, silent);
            } else {
                /* ★最可疑的静默丢包点：源端确有 frames > 0 个源帧，但转码**产出 0 字节**
                 *   ⇒ 该包被无声丢弃。典型成因：
                 *   ① 转码器配置非法（audioCapConvert 开头已记 lastError 并 warn）；
                 *   ② 管线刚建好时的第一包必然如此（首个源帧只作保持值初始化，不产出），
                 *      属正常，靠节流 + 上下文判断即可，不当错误处理；
                 *   ③ 源帧数太少、不足产出一个目标帧。
                 *   本轮只补**节流**告警：控制流（不写环形缓冲、直接返回）逐字未改。 */
                audioCapSetErrorf(self,
                    "audioCapHandlePacket: %d source frames produced 0 converted bytes "
                    "(src=%dHz/%dch/%dbit -> dst=%dHz/%dch/%dbit, passthrough=%d)",
                    (int)frames, pipe->srcRate, pipe->srcChannels, pipe->srcBits,
                    self->format.sampleRate, self->format.channels, self->format.bitsPerSample,
                    pipe->isPassthrough);
                if (audioCapThrottleDue(&self->lastDropWarnMs,
                                         AUDIOCAP_LOG_INTERVAL_MS, audioCapNowMs())) {
                    log_warn("audioCapHandlePacket: %d source frames produced 0 converted bytes "
                             "(src=%dHz/%dch/%dbit -> dst=%dHz/%dch/%dbit, passthrough=%d)",
                             (int)frames, pipe->srcRate, pipe->srcChannels, pipe->srcBits,
                             self->format.sampleRate, self->format.channels,
                             self->format.bitsPerSample, pipe->isPassthrough);
                }
            }
        }
    }
}

/* ---------------------------------------------------------------------
 * 采集线程体：CoInitialize -> 打开管线 -> 轮询采集 -> 释放管线 -> CoUninitialize
 * ------------------------------------------------------------------- */
static void audioCapWorker(void* param)
{
    PlatformAudioCaptureWindows* self = (PlatformAudioCaptureWindows*)param;
    AudioCapPipeline             pipe;
    PlatformSleepInterface       sleep;
    unsigned long                lastPacketMs;   /* 上次「取到包」的时刻（零包告警用） */
    unsigned long                startMs;        /* 进入采集主循环的时刻（节流基准） */

    if (self == NULL) {
        return;
    }

    sleep = NewPlatformSleep();

    if (audioCapPipelineOpen(self, &pipe) != 0) {
        self->captureError  = -1;
        self->pipelineState = -1;
        log_error("platform_audio_capture(WASAPI): pipeline open failed (srcType=%d device=%s)",
                  (int)self->config.sourceType,
                  (self->deviceId[0] != '\0') ? self->deviceId : "<default>");
        return;
    }

    self->captureError  = 0;
    self->isRunning     = 1;
    self->pipelineState = 1;

    /* ---- 可观测性用局部量（本轮新增；纯诊断，不参与任何控制流判断）----
     * lastPacketMs：上次「取到包」（GetNextPacketSize 报非零）的时刻；
     * startMs    ：进入采集主循环的时刻（用于首条统计日志的节流基准）。 */
    lastPacketMs = audioCapNowMs();
    startMs     = lastPacketMs;

    while (!self->isQuit) {
        UINT32  packet = 0;
        HRESULT hr = IAudioCaptureClient_GetNextPacketSize(pipe.captureClient, &packet);

        if (FAILED(hr)) {
            self->captureError = -2;
            audioCapSetErrorf(self, "IAudioCaptureClient_GetNextPacketSize failed hr=0x%08X",
                              (unsigned)hr);
            log_error("platform_audio_capture(WASAPI): GetNextPacketSize failed hr=0x%08X", (unsigned)hr);
            break;
        }

        if (packet != 0) {
            lastPacketMs = audioCapNowMs();   /* 本轮有包：刷新「上次有包」时刻 */
        }

        /* 环回模式不能用事件回调：在采集线程内轮询取包 */
        while (packet != 0 && !self->isQuit) {
            BYTE*  pData  = NULL;
            UINT32 nFrames = 0;
            DWORD  flags  = 0;

            hr = IAudioCaptureClient_GetBuffer(pipe.captureClient, &pData, &nFrames, &flags, NULL, NULL);
            if (FAILED(hr)) {
                self->captureError = -3;
                audioCapSetErrorf(self, "IAudioCaptureClient_GetBuffer failed hr=0x%08X",
                                  (unsigned)hr);
                log_error("platform_audio_capture(WASAPI): GetBuffer failed hr=0x%08X", (unsigned)hr);
                packet = 0;
                break;
            }

            if (!self->isPaused) {
                audioCapHandlePacket(self, &pipe, pData, nFrames,
                                     (flags & AUDCLNT_BUFFERFLAGS_SILENT) ? 1 : 0);
            }

            IAudioCaptureClient_ReleaseBuffer(pipe.captureClient, nFrames);

            hr = IAudioCaptureClient_GetNextPacketSize(pipe.captureClient, &packet);
            if (FAILED(hr)) {
                self->captureError = -2;
                audioCapSetErrorf(self, "IAudioCaptureClient_GetNextPacketSize failed hr=0x%08X",
                                  (unsigned)hr);
                packet = 0;
                break;
            }
        }

        if (self->isQuit) {
            break;
        }

        /* ---- 观测点 ①：周期「写入统计」（节流 AUDIOCAP_LOG_INTERVAL_MS）----
         * 放在 sleep 之前，保证「停不下来 / 一直有包」两种极端下节奏一致。 */
        audioCapWriteStatus(self, audioCapNowMs());

        /* ---- 观测点 ②：零包 / 端点状态告警（节流 AUDIOCAP_IDLE_WARN_MS）----
         * 用途：把「系统真的没出声音（正常静音 / 无播放）」与「端点被挂起或失效（异常）」
         * 区分开 —— 两者在 GetNextPacketSize 上都只表现为 packet == 0，COM 返回值仍是 S_OK。
         * GetCurrentState 属 IAudioClient 的 COM 方法，但 MinGW-w64 的 <audioclient.h>
         * **未实现**它（既无 objc-macro 也无 AUDIOCLIENT_STATE），故此处走本文件包含区之后
         * 自建的 vtbl 兼容层宏 IAudioClient_GetCurrentState（按 idx 15 取，详见该宏上方注释）。
         * 状态值用 int 承载（MinGW 无 AUDIOCLIENT_STATE 类型，定义同名 typedef 会与 MSVC SDK
         * 冲突），枚举含义（对照 SDK 头 audioclient.h 的 AUDIOCLIENT_STATE）:
         *   0 = AUDIOCLIENT_STATE_UNINITIALIZED（未初始化）
         *   1 = AUDIOCLIENT_STATE_ACTIVE（运行中；正常采集时应为此值）
         *   2 = AUDIOCLIENT_STATE_INACTIVE（设备无活动 / 已禁用）
         *   3 = AUDIOCLIENT_STATE_SUSPENDED（已挂起）
         * 顺序说明：**先过节流闸门、再查状态**，保证每条告警恰好一次 GetCurrentState
         * 调用（主循环 10 ms 一轮，绝不为观测而空转 COM）。 */
        {
            unsigned long nowMs   = audioCapNowMs();
            unsigned long idleMs  = (unsigned long)(nowMs - lastPacketMs);
            unsigned long sinceMs = (unsigned long)(nowMs - startMs);

            if (idleMs >= AUDIOCAP_IDLE_WARN_MS && sinceMs >= AUDIOCAP_IDLE_WARN_MS &&
                audioCapThrottleDue(&self->statIdleLastMs, AUDIOCAP_IDLE_WARN_MS, nowMs)) {
                int              state  = 0;   /* 0 = UNINITIALIZED；MinGW 无 AUDIOCLIENT_STATE，用 int 承载 */
                HRESULT          stateHr = IAudioClient_GetCurrentState(pipe.audioClient, &state);

                if (FAILED(stateHr)) {
                    /* 查询失败：退回 0（未初始化），即「状态未知」，仍打日志以便现场发现 */
                    state = 0;
                }
                log_warn("audioCap(WASAPI): no packets for %lu ms, endpointState=%d "
                         "(0=INVALID 1=ACTIVE 2=INACTIVE 3=SUSPENDED), device=%s",
                         idleMs, (int)state,
                         (self->deviceId[0] != '\0') ? self->deviceId : "<default>");
            }
        }

        sleep.sleepMs(AUDIOCAP_POLL_SLEEP_MS);
    }

    self->isRunning = 0;
    audioCapPipelineClose(&pipe);
    log_info("platform_audio_capture(WASAPI): capture thread exited "
             "(total written=%llu bytes in %lu packets, silent=%lu, lastError=\"%s\")",
             self->statBytes, self->statPackets, self->statSilentPackets, self->lastErrorText);
}

/* ---------------------------------------------------------------------
 * 内部前向声明（start 失败回滚时需调用 stop）
 * ------------------------------------------------------------------- */
static void audioCapDoStop(PlatformAudioCaptureWindows* self);

/* ---------------------------------------------------------------------
 * 枚举设备：不依赖 start()，可在未启动时调用（供配置页列出设备）。
 * 本函数在**调用线程**内自行 CoInitialize（与采集线程互不影响）。
 * ------------------------------------------------------------------- */
static int audioCaptureListDevices(void* instance, PlatformAudioSourceType type,
                                   PlatformAudioDeviceInfo* out, int outCap)
{
    IMMDeviceEnumerator* enumerator  = NULL;
    IMMDeviceCollection* collection  = NULL;
    IMMDevice*           defaultDev  = NULL;
    char                 defaultId[PLATFORM_AUDIO_DEVICE_ID_CAP];
    EDataFlow            flow;
    HRESULT              hr;
    UINT                 count = 0;
    UINT                 i;
    int                  written = 0;
    int                  needUninit = 0;

    ASSERT_NOT_NULL(instance, "platform_audio_capture(WASAPI) listDevices: instance is null", -1);
    ASSERT_NOT_NULL(out, "platform_audio_capture(WASAPI) listDevices: out is null", -2);

    if (outCap <= 0) {
        log_error("platform_audio_capture(WASAPI) listDevices: invalid outCap=%d", outCap);
        return -3;
    }
    if (type != PLATFORM_AUDIO_SOURCE_SYSTEM && type != PLATFORM_AUDIO_SOURCE_MIC) {
        log_error("platform_audio_capture(WASAPI) listDevices: invalid type=%d", (int)type);
        return -4;
    }

    defaultId[0] = '\0';
    flow = (type == PLATFORM_AUDIO_SOURCE_SYSTEM) ? eRender : eCapture;

    if (audioCapComInit(&needUninit) != 0) {
        log_error("platform_audio_capture(WASAPI) listDevices: CoInitializeEx failed");
        return -5;
    }

    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                          &IID_IMMDeviceEnumerator, (void**)&enumerator);
    if (FAILED(hr) || enumerator == NULL) {
        log_error("platform_audio_capture(WASAPI) listDevices: create MMDeviceEnumerator failed hr=0x%08X",
                  (unsigned)hr);
        if (needUninit) { CoUninitialize(); }
        return -6;
    }

    /* 该类型的默认设备 id（用于回填 isDefault） */
    if (SUCCEEDED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(enumerator, flow, eConsole, &defaultDev)) &&
        defaultDev != NULL) {
        LPWSTR idStr = NULL;
        if (SUCCEEDED(IMMDevice_GetId(defaultDev, &idStr)) && idStr != NULL) {
            audioCapWideToUtf8(idStr, defaultId, (int)sizeof(defaultId));
            CoTaskMemFree(idStr);
        }
    }

    hr = IMMDeviceEnumerator_EnumAudioEndpoints(enumerator, flow, DEVICE_STATE_ACTIVE, &collection);
    if (SUCCEEDED(hr) && collection != NULL &&
        SUCCEEDED(IMMDeviceCollection_GetCount(collection, &count))) {
        for (i = 0; i < count && written < outCap; i++) {
            IMMDevice*               dev   = NULL;
            LPWSTR                   idStr = NULL;
            IPropertyStore*          store = NULL;
            PROPVARIANT              pv;
            PlatformAudioDeviceInfo* info  = &out[written];

            if (FAILED(IMMDeviceCollection_Item(collection, i, &dev)) || dev == NULL) {
                continue;
            }

            memset(info, 0, sizeof(*info));
            info->sourceType = type;
            info->isDefault  = 0;

            if (SUCCEEDED(IMMDevice_GetId(dev, &idStr)) && idStr != NULL) {
                audioCapWideToUtf8(idStr, info->id, PLATFORM_AUDIO_DEVICE_ID_CAP);
                CoTaskMemFree(idStr);
            }
            if (info->id[0] != '\0' && defaultId[0] != '\0' && strcmp(info->id, defaultId) == 0) {
                info->isDefault = 1;
            }

            memset(&pv, 0, sizeof(pv));
            if (SUCCEEDED(IMMDevice_OpenPropertyStore(dev, STGM_READ, &store)) && store != NULL) {
                if (SUCCEEDED(IPropertyStore_GetValue(store, &PKEY_Device_FriendlyName, &pv))) {
                    if (pv.vt == VT_LPWSTR && pv.pwszVal != NULL) {
                        audioCapWideToUtf8(pv.pwszVal, info->name, PLATFORM_AUDIO_DEVICE_NAME_CAP);
                    }
                }
                PropVariantClear(&pv);   /* 不可漏：GetValue 成功时 PROPVARIANT 可能持有分配内存 */
                IPropertyStore_Release(store);
            }

            if (info->name[0] == '\0') {
                /* 友好名缺失时以 id 兜底，避免空串下拉项 */
                strncpy(info->name, info->id, PLATFORM_AUDIO_DEVICE_NAME_CAP - 1);
                info->name[PLATFORM_AUDIO_DEVICE_NAME_CAP - 1] = '\0';
            }

            IMMDevice_Release(dev);
            written++;
        }
    }

    if (collection != NULL) {
        IMMDeviceCollection_Release(collection);
    }
    if (defaultDev != NULL) {
        IMMDevice_Release(defaultDev);
    }
    if (enumerator != NULL) {
        IMMDeviceEnumerator_Release(enumerator);
    }
    if (needUninit) {
        CoUninitialize();
    }
    return written;
}

/* ---------------------------------------------------------------------
 * 启动：建线程对象 -> 注入线程体 -> 等待「管线就绪 / 失败」（有界等待）
 * ------------------------------------------------------------------- */
static int audioCapDoStart(PlatformAudioCaptureWindows* self)
{
    PlatformThreadConfig        cfg;
    PlatformSleepInterface      sleep;
    PlatformTimestampInterface  ts;
    uint64_t                    t0;
    int                         err;

    self->isQuit         = 0;
    self->isPaused       = 0;
    self->captureError   = 0;
    self->pipelineState  = 0;
    self->overflowWarned = 0;

    /* 可观测性状态复位（与 overflowWarned 同处一并做）：统计**从 0 开始**，
     * 节流时刻也一并清零，使每个 run 的「写入统计」都从同一基线起算。
     * 注：statLastMs / statIdleLastMs 等清零后，首条日志因 nowMs 通常远大于
     *     AUDIOCAP_LOG_INTERVAL_MS 而**立即放行一次**（期望行为：开跑即见基线）。 */
    self->statBytes            = 0;
    self->statPackets          = 0;
    self->statSilentPackets    = 0;
    self->statDeltaBytes       = 0;
    self->statDeltaPackets     = 0;
    self->statDeltaSilentPackets = 0;
    self->statLastMs           = 0;
    self->statIdleLastMs       = 0;
    self->lastReadLogMs        = 0;
    self->lastInvalidCfgMs     = 0;
    self->lastFullWarnMs       = 0;
    self->lastDropWarnMs       = 0;

    memset(&cfg, 0, sizeof(cfg));
    cfg.stackSize = 0;    /* 0 = 平台默认 */
    cfg.priority  = 0;
    cfg.name      = NULL;
    cfg.workFunc  = NULL; /* 线程体与实参由 interfaces.start 注入 */
    cfg.workParam = NULL;

    self->thread = NewPlatformThreadObject(&cfg);
    if (self->thread == NULL) {
        log_error("platform_audio_capture: create thread object failed (device=%s)",
                  (self->deviceId[0] != '\0') ? self->deviceId : "<default>");
        return -1;
    }

    err = self->thread->interfaces.start(self->thread->instance, audioCapWorker, self);
    if (err != 0) {
        log_error("platform_audio_capture: thread start failed");
        DeleteThreadObject(self->thread);
        self->thread = NULL;
        return -2;
    }

    /* 有界等待采集线程拉起管线（成功 1 / 失败 -1），使 start() 能同步上报真实结果 */
    sleep = NewPlatformSleep();
    ts    = NewPlatformTimestamp();
    t0    = ts.milliseconds();
    while (self->pipelineState == 0) {
        if ((ts.milliseconds() - t0) >= AUDIOCAP_STREAM_INIT_TIMEOUT_MS) {
            break;
        }
        sleep.sleepMs(2);
    }

    if (self->pipelineState != 1) {
        log_error("platform_audio_capture: pipeline not ready (state=%d)", self->pipelineState);
        audioCapDoStop(self);
        return -3;
    }

    self->started = 1;
    log_info("platform_audio_capture: started (srcType=%d device=%s rate=%d ch=%d bits=%d)",
             (int)self->config.sourceType,
             (self->deviceId[0] != '\0') ? self->deviceId : "<default>",
             self->format.sampleRate, self->format.channels, self->format.bitsPerSample);
    return 0;
}

/* ---------------------------------------------------------------------
 * 停止：置退出标志 -> 等线程自然退出（有界）-> 超时兜底强停 -> 销毁线程对象。
 * 幂等；**保留**环形缓冲内已采集数据。
 * 关于可观测性统计（stat*）：**刻意不清零**。停流后调用方仍可经 lastError() 取到
 * 最近一次失败原因（纯诊断，不随 stop 失效），而「本次 run 采到多少字节 / 多少包 /
 * 多少静音包」正是停流现场最需要的证据；统计会在下一次 audioCapDoStart() 内复位。
 * ------------------------------------------------------------------- */
static void audioCapDoStop(PlatformAudioCaptureWindows* self)
{
    PlatformSleepInterface     sleep;
    PlatformTimestampInterface ts;
    uint64_t                   t0;

    if (self->thread == NULL) {
        self->started   = 0;
        self->isRunning = 0;
        return;
    }

    /* 先置退出标志让线程自然退出（不得直接强杀） */
    self->isQuit   = 1;
    self->isPaused = 0;

    sleep = NewPlatformSleep();
    ts    = NewPlatformTimestamp();
    t0    = ts.milliseconds();
    while (self->thread->interfaces.isRun(self->thread->instance) != 0 &&
           (ts.milliseconds() - t0) < AUDIOCAP_STREAM_STOP_TIMEOUT_MS) {
        sleep.sleepMs(2);
    }

    if (self->thread->interfaces.isRun(self->thread->instance) != 0) {
        log_warn("platform_audio_capture: wait stop timeout, force terminate capture thread");
        self->thread->interfaces.stop(self->thread->instance);
    }

    DeleteThreadObject(self->thread);
    self->thread    = NULL;
    self->isRunning = 0;
    self->started   = 0;
}

/* ---------------------------------------------------------------------
 * 对外方法（装配到 PlatformAudioCapture 虚表）
 * ------------------------------------------------------------------- */
static int audioCaptureStart(void* instance)
{
    PlatformAudioCaptureWindows* self = (PlatformAudioCaptureWindows*)instance;

    ASSERT_NOT_NULL(instance, "platform_audio_capture(WASAPI) start: instance is null", -1);

    if (self->started) {
        return 0;   /* 幂等：已启动 */
    }
    return audioCapDoStart(self);
}

static int audioCapturePause(void* instance)
{
    PlatformAudioCaptureWindows* self = (PlatformAudioCaptureWindows*)instance;

    ASSERT_NOT_NULL(instance, "platform_audio_capture(WASAPI) pause: instance is null", -1);

    self->isPaused = 1;   /* 线程继续存活，仅停止写入环形缓冲；保留已有数据 */
    return 0;
}

static int audioCaptureResume(void* instance)
{
    PlatformAudioCaptureWindows* self = (PlatformAudioCaptureWindows*)instance;

    ASSERT_NOT_NULL(instance, "platform_audio_capture(WASAPI) resume: instance is null", -1);

    self->isPaused = 0;
    return 0;
}

static int audioCaptureClear(void* instance)
{
    PlatformAudioCaptureWindows* self = (PlatformAudioCaptureWindows*)instance;

    ASSERT_NOT_NULL(instance, "platform_audio_capture(WASAPI) clear: instance is null", -1);

    audioCapLock(self);
    self->ringRead  = 0;
    self->ringWrite = 0;
    self->ringUsed  = 0;
    audioCapUnlock(self);
    return 0;
}

static int audioCaptureStop(void* instance)
{
    PlatformAudioCaptureWindows* self = (PlatformAudioCaptureWindows*)instance;

    ASSERT_NOT_NULL(instance, "platform_audio_capture(WASAPI) stop: instance is null", -1);

    if (!self->started && self->thread == NULL) {
        return 0;   /* 幂等：未启动 */
    }
    audioCapDoStop(self);
    return 0;
}

static int audioCaptureAvailableBytes(void* instance)
{
    PlatformAudioCaptureWindows* self = (PlatformAudioCaptureWindows*)instance;
    int n;

    ASSERT_NOT_NULL(instance, "platform_audio_capture(WASAPI) availableBytes: instance is null", -1);

    audioCapLock(self);
    n = self->ringUsed;
    audioCapUnlock(self);
    return n;
}

static int audioCaptureSetSampleRate(void* instance, int sampleRateHz)
{
    PlatformAudioCaptureWindows* self = (PlatformAudioCaptureWindows*)instance;
    PlatformAudioCaptureFormat   oldFormat;
    int                          wasStarted;

    ASSERT_NOT_NULL(instance, "platform_audio_capture(WASAPI) setSampleRate: instance is null", -1);

    if (sampleRateHz <= 0) {
        log_error("platform_audio_capture(WASAPI) setSampleRate: invalid sampleRateHz=%d", sampleRateHz);
        return -2;
    }
    if (sampleRateHz == self->format.sampleRate) {
        return 0;   /* 无变化 */
    }

    oldFormat  = self->format;
    wasStarted = self->started;

    if (wasStarted) {
        audioCapDoStop(self);   /* 停流（保留缓冲内数据） */
    }

    /* 按新格式重建采集管线（实际设备打开在采集线程内进行） */
    self->format.sampleRate = sampleRateHz;

    if (wasStarted) {
        if (audioCapDoStart(self) != 0) {
            log_error("platform_audio_capture(WASAPI) setSampleRate(%d) 重建失败，回滚到 %d",
                      sampleRateHz, oldFormat.sampleRate);
            self->format = oldFormat;
            if (audioCapDoStart(self) != 0) {
                log_error("platform_audio_capture(WASAPI): 回滚后重新启动仍失败，采集保持停止");
            }
            return -3;
        }
    }

    log_info("platform_audio_capture(WASAPI): sample rate set to %d", sampleRateHz);
    return 0;
}

static int audioCaptureGetFormat(void* instance, PlatformAudioCaptureFormat* outFormat)
{
    PlatformAudioCaptureWindows* self = (PlatformAudioCaptureWindows*)instance;

    ASSERT_NOT_NULL(instance, "platform_audio_capture(WASAPI) getFormat: instance is null", -1);
    ASSERT_NOT_NULL(outFormat, "platform_audio_capture(WASAPI) getFormat: outFormat is null", -2);

    *outFormat = self->format;
    return 0;
}

static int audioCaptureRead(void* instance, void* out, int outCap)
{
    PlatformAudioCaptureWindows* self = (PlatformAudioCaptureWindows*)instance;
    int avail;
    int n;
    int contig;
    int minChunk;

    ASSERT_NOT_NULL(instance, "platform_audio_capture(WASAPI) read: instance is null", -1);
    ASSERT_NOT_NULL(out, "platform_audio_capture(WASAPI) read: out is null", -2);

    if (outCap <= 0) {
        return -3;
    }
    if (self->captureError < 0) {
        return self->captureError;   /* 采集出错：上报负值 */
    }

    audioCapLock(self);
    avail    = self->ringUsed;
    minChunk = self->format.minChunkBytes;

    /* 可用字节数不足 minChunkBytes：返回 0（暂无可读，不阻塞）。
     * 【观测点 ③】这是**合法返回码 0**（按头文件契约不是错误），但消费者视角
     * 与「采到 0 字节」难以区分，现场只能表现为「一直没输出也没日志」。
     * 故补一条**严格节流**的 info：read() 由消费者高频调用（上层每 100 ms 一次），
     * 绝不能逐次打日志。值在锁内取快照、**解锁后**再打，避免持锁做日志 I/O。 */
    if (avail <= 0 || avail < minChunk) {
        int           ringUsedSnap = self->ringUsed;
        int           ringCapSnap  = self->ringCap;
        unsigned long nowMs        = audioCapNowMs();

        audioCapUnlock(self);

        if (audioCapThrottleDue(&self->lastReadLogMs, AUDIOCAP_LOG_INTERVAL_MS, nowMs)) {
            int bytesPerSec = self->format.sampleRate *
                              self->format.channels *
                              (self->format.bitsPerSample / 8);
            /* 中间量用 long long：minChunkBytes 由调用方给定，*1000 用 int 乘法有溢出风险 */
            int minChunkMs  = (minChunk > 0 && bytesPerSec > 0)
                              ? (int)(((long long)minChunk * 1000LL) / (long long)bytesPerSec) : 0;

            log_info("audioCaptureRead: avail=%d < minChunk=%d (= %d ms of audio), "
                     "ring=%d/%d, running=%d, paused=%d",
                     avail, minChunk, minChunkMs, ringUsedSnap, ringCapSnap,
                     (int)self->isRunning, (int)self->isPaused);
        }
        return 0;
    }

    n = (outCap < avail) ? outCap : avail;

    /* 一次只交连续的一段：拷贝语义，绝不暴露回绕 */
    contig = self->ringCap - self->ringRead;
    if (n > contig) {
        n = contig;
    }
    if (n <= 0) {
        audioCapUnlock(self);
        return 0;
    }

    memcpy(out, self->ring + self->ringRead, (size_t)n);
    self->ringRead = (self->ringRead + n) % self->ringCap;
    self->ringUsed -= n;
    audioCapUnlock(self);
    return n;
}

/* 释放具体实例（幂等）：先停采集线程，再释放缓冲 / 互斥量 / 实例 */
static void audioCaptureDeleteInstance(void* instance)
{
    PlatformAudioCaptureWindows* self;

    if (instance == NULL) {
        return;
    }
    self = (PlatformAudioCaptureWindows*)instance;

    audioCapDoStop(self);

    if (self->mutex != NULL) {
        DeletePlatformMutex(&self->mutex);
    }
    if (self->ring != NULL) {
        audioCaptureFree(self->ring);
        self->ring = NULL;
    }
    self->ringCap = 0;

    audioCaptureFree(self);
}

/* 最近一次失败的可读原因（契约：指向实例内部只读缓冲的 NUL 结尾字符串；无错误时
 * 返回空串 ""；**绝不返回 NULL**；调用方只读、不得 free、不得改写）。
 * ⚠️ deleteInstance 之后本函数**不得**再被调用（实例已释放）。 */
static const char* audioCaptureLastError(void* instance)
{
    PlatformAudioCaptureWindows* self = (PlatformAudioCaptureWindows*)instance;

    if (self == NULL) {
        return "";     /* 契约：绝不返回 NULL */
    }
    return self->lastErrorText;
}

/* 集中返回虚表，避免手写漏项。
 * 【装配约定】deleteInstance 保持为**最后一项**（工程硬约定），lastError 插在它之前。 */
#define _RETURN_PLATFORM_AUDIO_CAPTURE_INTERFACE_WINDOWS \
PlatformAudioCaptureInterface interfaces = {              \
        .listDevices    = audioCaptureListDevices,        \
        .start          = audioCaptureStart,              \
        .pause          = audioCapturePause,              \
        .resume         = audioCaptureResume,             \
        .clear          = audioCaptureClear,              \
        .stop           = audioCaptureStop,               \
        .availableBytes = audioCaptureAvailableBytes,     \
        .setSampleRate  = audioCaptureSetSampleRate,      \
        .getFormat      = audioCaptureGetFormat,          \
        .read           = audioCaptureRead,               \
        .lastError      = audioCaptureLastError,          \
        .deleteInstance = audioCaptureDeleteInstance      \
    };                                                    \
return interfaces;

static PlatformAudioCaptureInterface audioCaptureWindowsInterface(void)
{
    _RETURN_PLATFORM_AUDIO_CAPTURE_INTERFACE_WINDOWS
}

/* Windows 具体构造：校验配置 -> 装配具体实例（缓冲 / 互斥量）-> 调抽象构造完成包装
 * （禁止手工赋值 instance / interfaces）。零默认值：任何缺项即构造失败。 */
static PlatformAudioCapture* NewPlatformAudioCaptureWindows(const PlatformAudioCaptureConfig* config)
{
    PlatformAudioCaptureWindows* self;
    PlatformAudioCapture*        capture;

    if (config == NULL) {
        log_error("NewPlatformAudioCaptureNative: config is null");
        return NULL;
    }
    if (config->sourceType != PLATFORM_AUDIO_SOURCE_SYSTEM &&
        config->sourceType != PLATFORM_AUDIO_SOURCE_MIC) {
        log_error("NewPlatformAudioCaptureNative: invalid sourceType=%d", (int)config->sourceType);
        return NULL;
    }
    if (config->format.sampleRate <= 0) {
        log_error("NewPlatformAudioCaptureNative: invalid sampleRate=%d", config->format.sampleRate);
        return NULL;
    }
    if (config->format.channels != 1 && config->format.channels != 2) {
        log_error("NewPlatformAudioCaptureNative: invalid channels=%d", config->format.channels);
        return NULL;
    }
    if (config->format.bitsPerSample != 16 && config->format.bitsPerSample != 32) {
        log_error("NewPlatformAudioCaptureNative: invalid bitsPerSample=%d", config->format.bitsPerSample);
        return NULL;
    }
    if (config->ringBytes <= 0) {
        log_error("NewPlatformAudioCaptureNative: invalid ringBytes=%d", config->ringBytes);
        return NULL;
    }

    self = (PlatformAudioCaptureWindows*)audioCaptureMalloc(sizeof(PlatformAudioCaptureWindows));
    ASSERT_NOT_NULL(self, "NewPlatformAudioCaptureWindows: audioCaptureMalloc failed", NULL);
    memset(self, 0, sizeof(PlatformAudioCaptureWindows));   /* 零基线：全部成员（含本轮新增的
                                                            * 统计 / 节流 / lastErrorText）已清零 */

    self->config = *config;
    self->format = config->format;
    self->ringCap = config->ringBytes;
    self->overflowWarned = 0;
    self->lastError = 0;
    /* lastError 文本缓冲：契约要求「无错误时返回空串」，故显式置空（memset 已覆盖，
     * 此处显式再置一次以表明这是**有意为之**的初值，不依赖 memset 的副作用） */
    self->lastErrorText[0] = '\0';

    /* 可观测性成员：memset 已把累计值与节流时刻全部清零，此处**显式**再写一遍
     * stat* 与全部节流时刻，确保「构造期基线」不依赖任何隐含前提。
     * statLastMs / statIdleLastMs / last*Ms 刻意保持 0：0 意味着「从未打点」，
     * 故首条日志会立即放行一次（开跑即见基线），之后按 AUDIOCAP_*_MS 节流。 */
    self->statBytes              = 0;
    self->statPackets            = 0;
    self->statSilentPackets      = 0;
    self->statDeltaBytes         = 0;
    self->statDeltaPackets       = 0;
    self->statDeltaSilentPackets = 0;
    self->statLastMs             = 0;
    self->statIdleLastMs         = 0;
    self->lastReadLogMs          = 0;
    self->lastInvalidCfgMs       = 0;
    self->lastFullWarnMs         = 0;
    self->lastDropWarnMs         = 0;

    if (config->deviceId != NULL && config->deviceId[0] != '\0') {
        strncpy(self->deviceId, config->deviceId, PLATFORM_AUDIO_DEVICE_ID_CAP - 1);
        self->deviceId[PLATFORM_AUDIO_DEVICE_ID_CAP - 1] = '\0';
    } else {
        self->deviceId[0] = '\0';
    }
    /* 配置里的 deviceId 指针不再持有（改用内部副本），避免调用方字符串生命周期问题 */
    self->config.deviceId = NULL;

    self->ring = (unsigned char*)audioCaptureMalloc((size_t)self->ringCap);
    if (self->ring == NULL) {
        log_error("NewPlatformAudioCaptureWindows: alloc ring failed (%d bytes)", self->ringCap);
        audioCaptureFree(self);
        return NULL;
    }

    self->mutex = NewPlatformMutexForPlatform();
    if (self->mutex == NULL) {
        log_error("NewPlatformAudioCaptureWindows: create mutex failed");
        audioCaptureFree(self->ring);
        self->ring = NULL;
        audioCaptureFree(self);
        return NULL;
    }

    capture = NewPlatformAudioCapture(self, audioCaptureWindowsInterface());
    if (capture == NULL) {
        audioCaptureFree(self->ring);
        self->ring = NULL;
        DeletePlatformMutex(&self->mutex);
        audioCaptureFree(self);
        return NULL;
    }

    if (config->startOnCreate) {
        if (capture->interfaces.start(capture->instance) != 0) {
            log_error("NewPlatformAudioCaptureNative: startOnCreate requested but start failed");
            DeletePlatformAudioCapture(&capture);
            return NULL;
        }
    }

    return capture;
}

/* 平台中立工厂：Windows -> WASAPI 具象 */
PlatformAudioCapture* NewPlatformAudioCaptureNative(const PlatformAudioCaptureConfig* config)
{
    return NewPlatformAudioCaptureWindows(config);
}

#else /* 非 Windows 平台 */

/* 保证任何平台都能链接通过：工厂返回 NULL（并 log_error）。
 * 抽象构造 / DeletePlatformAudioCapture 仍然可用（上层可自行注入自备的具体实例）。 */
PlatformAudioCapture* NewPlatformAudioCaptureNative(const PlatformAudioCaptureConfig* config)
{
    (void)config;
    log_error("NewPlatformAudioCaptureNative: audio capture not available on this platform");
    return NULL;
}

#endif /* 平台互斥分支 */

#endif /* PLATFORM_USE_AUDIO_CAPTURE */
