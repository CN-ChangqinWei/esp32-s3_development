#include "platform_websocket.h"
#include "assert_marco.h"
#include <string.h>
#include <stddef.h>

/* =====================================================================
 * platform_websocket —— 跨平台「WebSocket 客户端」能力实现（可轮询对象式）
 *
 * 结构（自顶向下）：
 *   1) 能力开关 PLATFORM_USE_WEBSOCKET：关闭时不生成任何函数；
 *   2) 抽象层（与平台无关，能力开启即无条件生成）：
 *        NewPlatformWebSocket / InitPlatformWebSocket / DeletePlatformWebSocket；
 *   3) 平台互斥分支（同一编译单元只命中一个）：
 *        PLATFORM_WINDOWS -> WinHTTP WebSocket 具象 + NewPlatformWebSocketNative
 *        #else（Linux / macOS / FreeRTOS）-> NewPlatformWebSocketNative 返回 NULL（保证可链接）
 *
 * 内存：整个模块的堆分配/释放统一走同模块能力 NewPlatformMemory()
 *       （见 platform_memory.h）；若编译期关闭了内存能力（PLATFORM_USE_MEMORY==0），
 *       为不与本文件的自依赖冲突，退回 <stdlib.h> 的 malloc/free，并在此注明原因。
 *
 * 平台隔离：具体实现结构体（PlatformWebSocketWindows）定义在本 .c 的文件作用域内，
 *       绝不写进头文件；**严禁**把 <windows.h> / <winhttp.h> 放进
 *       platform_websocket.h —— 该头会被 C++ 的 Qt 编译单元间接包含，一旦引入平台头，
 *       其 COM 宏 `#define interface struct` 与 IN/OUT/TRUE/FALSE 等宏会污染整个工程。
 *       这是对「具体实现结构体放头文件」规范的**有意偏离**。
 *
 * ---------------------------------------------------------------------
 * WinHTTP WebSocket API 的调用路线（本实现的选择与理由）：
 * ---------------------------------------------------------------------
 *   winhttp.h 里 WebSocket 相关的**函数声明与枚举**被 `#if(_WIN32_WINNT >= 0x0602)`
 *   包住；而本工程禁止在源码里重定义 _WIN32_WINNT（既有 timestamp.c 已因此产生
 *   预存量「宏重定义」告警，不再引入第二处）。为保证「任意 SDK 版本宏组合下都能
 *   编译通过」，本文件：
 *     - **不直接引用 WinHttpWebSocket* 符号**，改为 GetProcAddress 从 winhttp.dll
 *       动态解析（winhttp 已由链接器导入 winhttp.lib 保证加载）；
 *     - **不引用 WINHTTP_WEB_SOCKET_* 枚举名**，改用本文件内自定义的 ABI 常量
 *       PLATFORM_WS_BT_*（数值与 SDK 枚举逐一对应，见下方定义处注释）。
 *   其余 WinHttp* 函数（Open / Connect / OpenRequest / SetOption / SendRequest /
 *   ReceiveResponse / CloseHandle / SetTimeouts / AddRequestHeaders）**不带**
 *   版本宏条件，直接调用即可。
 *
 * 命名注意：
 *   - 本文件为纯 C，实例局部变量一律命名 self（禁用 `this`）；
 *   - 具象函数一律带 ws 前缀（wsConnect / wsClose ...）：Windows SDK 会声明
 *     同名的 `connect`、且 <windows.h> 带来大量短名宏，加前缀彻底规避重名与宏展开。
 *
 * =====================================================================
 * 收帧模型（本模块的**核心设计**：把不可中断的阻塞 Receive 隔离到专用接收线程）
 * =====================================================================
 * 【已取证的事实（为什么必须这么做）】
 *   1) WinHttpSetTimeouts **不接受 WebSocket 句柄**：对 hWebSocket 调用返回 FALSE 且
 *      GetLastError()==12018（ERROR_WINHTTP_INCORRECT_HANDLE_TYPE）；
 *   2) WinHttpWebSocketCompleteUpgrade 新建的 WS 句柄**不继承** session 超时
 *      （即便在 CompleteUpgrade 之前把 session 超时设得很小，实测仍无效）。
 *   ⟹ WinHttpWebSocketReceive 是**不可中断、不可设超时**的阻塞调用；任何「预期为空的
 *      poll」（例如「收齐后再泵一次确认」）都会**永久卡死**（本项目 vosk 单元曾如此）。
 *
 * 【方案 X（已批准）：接收线程只做「原始接收结果入队」，状态机仍留在调用方线程】
 *   · 接收线程（wsRecvThreadBody）：循环调用 dynReceive（可任意久地阻塞）+ 把
 *     (err, bufferType, bytesRead, 原始字节) 拷进**固定大小事件槽**的环形队列；
 *     队列满 ⇒ 丢**最旧** + 节流 log_error + 置「已丢帧」标志（帧连续性已破坏，
 *     后续 pollFrame **必须**上报为错误，绝不静默继续）；
 *     退出条件 = recvStop 置位 或 Receive 返回致命错误（连接断开）。
 *   · pollFrame（调用方线程）：从队列**按 pollSliceMs 有界取出**一个事件，并推进原有的
 *     「帧累积 / 分片交付」状态机；队列空且等待超过 pollSliceMs ⇒ 返回 0。
 *   这样做的**最大好处**：frameBuf / frameLen / deliverPos / frameReady / closeFramePending /
 *     frameType 以及 lastError / pollStopped 等实例状态**全部仍只在调用方线程读写**，
 *     因此它们**无需加锁**；需要同步的**只有队列本体** ⇒ 引入新并发 bug 的面最小。
 *
 * 【sendFrame 与接收线程并发是否安全】
 *   本设计下 WinHttpWebSocketSend 只在**调用方线程**调用，WinHttpWebSocketReceive 只在
 *   **接收线程**调用：二者是同一 WS 句柄上**不同方向**的 I/O，不属同向并发。
 *   （「并发 send/recv 会得到 1006」的经验来自 Python websocket-client 库 —— 其 socket
 *   抽象在库内部对同一连接加了额外约束，不适用于 WinHTTP。）
 *   【最小回退手段】万一日后发现该并发确实不被 WinHTTP 允许：把接收线程禁用、退回
 *   「pollFrame 内直接 Receive」的旧直连模式即可（不启动接收线程、pollFrame 直接调
 *   dynReceive，队列/停止逻辑一并去掉）。保留此说明以便快速回退。
 *
 * 【停止路径】置 recvStop → 关掉 WS 句柄（**唯一**能中断阻塞 Receive 的手段）→
 *   带超时的 join → 才做既有清理。并发 CloseHandle 的权衡见 wsStopReceiveThread 内注释。
 *
 * 【能力依赖】本改造后本模块依赖 PLATFORM_USE_THREAD_OBJECT / PLATFORM_USE_SYNC /
 *   PLATFORM_USE_SLEEP（与 platform_audio_capture 一致；三者默认开启）。
 * ===================================================================== */

#if !PLATFORM_USE_WEBSOCKET

/* 能力关闭：不生成任何函数。
 * platform_websocket.h 中的 typedef 仍然存在（本编译单元非空），无需额外占位。 */

#else /* PLATFORM_USE_WEBSOCKET */

/* ---------------------------------------------------------------------
 * 本模块内部常量
 *   —— 只有两类：① 定长缓冲容量上界；② WebSocket 协议常量。
 *      绝无「默认 host / 默认端口 / 默认 path」（库内零默认值，全部外部注入）。
 * ------------------------------------------------------------------- */
#define PLATFORM_WS_HOST_BUF_SIZE            256     /* host 缓冲容量（含结尾 '\0'） */
#define PLATFORM_WS_PATH_BUF_SIZE           2048     /* path 缓冲容量（含结尾 '\0'） */
#define PLATFORM_WS_QUERY_BUF_SIZE          2048     /* query 缓冲容量（含结尾 '\0'） */
#define PLATFORM_WS_HEADER_NAME_BUF_SIZE     128     /* 额外握手头「名」缓冲容量 */
#define PLATFORM_WS_HEADER_VALUE_BUF_SIZE   1024     /* 额外握手头「值」缓冲容量 */
/* 请求目标 = path + '?' + query，故容量上界为两者之和再多 2 字节余量 */
#define PLATFORM_WS_TARGET_BUF_SIZE \
        (PLATFORM_WS_PATH_BUF_SIZE + PLATFORM_WS_QUERY_BUF_SIZE + 2)
#define PLATFORM_WS_ERR_BUF_SIZE             512     /* lastError 文本缓冲容量 */
#define PLATFORM_WS_RECV_CHUNK_SIZE         8192     /* 单次底层接收的缓冲（接收线程栈上）＝ 单事件槽的载荷上限 */
#define PLATFORM_WS_SEND_CHUNK_SIZE       (64 * 1024) /* 单次底层发送的分片上限（超过则按 MESSAGE+FRAGMENT 切分） */
#define PLATFORM_WS_FRAME_INITIAL_CAP       4096     /* 收帧累积缓冲的初始容量（按需倍增） */

/* 「单次 pollFrame 阻塞上界」的保底下限（毫秒）。
 *
 * 背景：connect 在握手成功后会把会话超时切换为 pollSliceMs（使单次 pollFrame 有界）。
 * 若该 WinHttpSetTimeouts 调用失败，会话 receive timeout 会**退回握手阶段的 connectTimeoutMs**
 * （调用方常配成 60000 级）⇒ 单次 pollFrame 可能长时间不返回、轮询节流失效。此时本模块再
 * 用本「保底值」重设一次，目的**不是**恢复节流精度，而是把「无界/极长」收敛为「有界」。
 *
 * 取值取舍：
 *   - 太小（如几十毫秒）：会把「对端稍慢的正常响应」误判成超时，放大无效重连；
 *   - 太大：等于没有收敛阻塞（回到原点）；
 *   - 1000 ms 是「确保有界」与「不误伤正常帧」之间的折中。
 * 注意它只是**下限**：若 pollSliceMs 本身更大，保底仍沿用更大的 pollSliceMs
 * （见 wsConnect 中 fallbackMs 的取值）。
 *
 * 【改造后说明】本保底值作用于**会话级（WinHTTP 侧）**的 I/O 上界；pollFrame 的「有界」现
 * 已由专用接收线程 + 内部队列保证（真实上界 = pollSliceMs），与本值无关。此处措辞保留
 * M17 当时的历史语境，代码与三段超时设置**均未改动**。 */
#define PLATFORM_WS_POLL_WAIT_FALLBACK_MIN_MS 1000

/* 「手段 2」：对 WS 句柄单独设置单次 I/O 上界时，在 pollSliceMs 基础上放的裕量倍数。
 * 取 4 ⇒ pollSliceMs=1000 时 WS 句柄上界为 4000 ms：既让「对端稍慢的正常响应」不被
 * 误判为超时，又不至于把上界放大到失去「有界」的意义。 */
#define PLATFORM_WS_WS_HANDLE_TIMEOUT_FACTOR 4

/* 「手段 3」：单次（阻塞）WinHttpWebSocketReceive 实际耗时的观测量参数。
 *   - ELAPSED_LOG_MIN_MS ：低于此耗时的**返回**不打日志（数据流畅时 Receive 很快返回，
 *                          全打会刷屏）；
 *   - LOG_THROTTLE_MS    ：耗时类日志的节流间隔（同一会话内最多每 N 毫秒一条）。
 * 改造后语义（重要）：Receive 已由**专用接收线程**承载，长等待（对端久不发言）是该调用的
 * **正常**行为（它本就不可设超时），故本日志只作诊断性的 log_info，不再当作「超时未生效」
 * 的错误上报（原 OVERRUN_MARGIN_MS + 「超界 log_error」分支已随之移除）。 */
#define PLATFORM_WS_RECV_ELAPSED_LOG_MIN_MS    100
#define PLATFORM_WS_RECV_LOG_THROTTLE_MS      1000

/* ---- 专用接收线程 + 内部事件队列（把不可中断的阻塞 Receive 隔离到独立线程） ---- */

/* 事件槽数组深度。单槽 = sizeof(PlatformWsRecvEvent) ≈ 8 KiB（含 8192 字节载荷），
 * 64 ⇒ 约 512 KiB。取值理由：
 *   - 这是「对端一次性突发推送」时的缓冲余量：本项目为流式（按帧消费），正常节奏下队列
 *     通常只有 0~1 个事件；出现几十个滞留事件即说明调用方没有及时 pollFrame；
 *   - 小于 16 会在调度抖动 / 调用方偶发卡顿时过早丢帧（丢帧 = 连接不可用，代价高）；
 *   - 大于 128 只是徒增内存（每槽 8 KiB），对「只读诊断」场景没有收益。
 * 到达该深度即丢**最旧**并置「已丢帧」标志（见 queueDropped）。 */
#define PLATFORM_WS_QUEUE_DEPTH                64

/* 队列空时 pollFrame 的等待轮询分片（毫秒）。取 10：
 *   - 足够小：pollSliceMs 到点返回的额外延迟 ≤ 一个分片；
 *   - 足够大：pollSliceMs=1000 时只轮询约 100 次，CPU 开销可忽略；
 *   - 与 platform_audio_capture 的 AUDIOCAP_POLL_SLEEP_MS(10) 同量级，风格一致。 */
#define PLATFORM_WS_QUEUE_POLL_SLICE_MS        10

/* 关闭时等待接收线程**自然退出**的上限（毫秒）。关掉 WS 句柄后 Receive 会立刻返回并
 * 跳出循环，正常只需几毫秒；3000 ms 只用于兜住「系统未按预期中断阻塞调用」的极端情形，
 * 避免 close() 出现无界等待。 */
#define PLATFORM_WS_RECV_THREAD_JOIN_MS      3000

/* 「等线程进入阻塞 Receive」与「等 isRun 落零」的短等待上限（毫秒）。取 200：
 * 足够覆盖线程调度延迟，又不会让 close() 出现可感知停顿。 */
#define PLATFORM_WS_RECV_SETTLE_WAIT_MS       200

/* 「队列满丢帧」log_error 的节流间隔（毫秒）。 */
#define PLATFORM_WS_QUEUE_DROP_LOG_THROTTLE_MS 1000

/* ---------------------------------------------------------------------
 * 内存：统一走同模块能力 NewPlatformMemory()
 * ------------------------------------------------------------------- */
#if PLATFORM_USE_MEMORY

static void* websocketMalloc(size_t size)
{
    PlatformMemoryInterface mem = NewPlatformMemory();
    return mem.malloc(size);
}

static void websocketFree(void* ptr)
{
    PlatformMemoryInterface mem = NewPlatformMemory();
    mem.free(ptr);
}

#else /* PLATFORM_USE_MEMORY == 0 */

/* 内存能力被裁剪时 NewPlatformMemory 不可用。本模块必须能分配抽象对象与具体
 * 实例，故按规范允许的例外退回标准库 malloc/free（仅此一处，且为唯一退路）。 */
#include <stdlib.h>

static void* websocketMalloc(size_t size)
{
    return malloc(size);
}

static void websocketFree(void* ptr)
{
    free(ptr);
}

#endif /* PLATFORM_USE_MEMORY */

/* =====================================================================
 * 抽象层（与平台无关，能力开启即无条件生成）
 * ===================================================================== */

PlatformWebSocket* NewPlatformWebSocket(void* instance, PlatformWebSocketInterface interfaces)
{
    PlatformWebSocket* socket;

    ASSERT_NOT_NULL(instance, "NewPlatformWebSocket: instance is null", NULL);

    socket = (PlatformWebSocket*)websocketMalloc(sizeof(PlatformWebSocket));
    ASSERT_NOT_NULL(socket, "NewPlatformWebSocket: websocketMalloc failed", NULL);

    /* 交由 Init 完成装配（只设置 instance/interfaces，禁止手工赋值） */
    if (InitPlatformWebSocket(socket, instance, interfaces) != 0) {
        websocketFree(socket);
        socket = NULL;
        return socket;
    }

    return socket;
}

int InitPlatformWebSocket(PlatformWebSocket* socket, void* instance,
                          PlatformWebSocketInterface interfaces)
{
    ASSERT_NOT_NULL(socket, "InitPlatformWebSocket: socket is null", -1);
    ASSERT_NOT_NULL(instance, "InitPlatformWebSocket: instance is null", -2);

    socket->instance   = instance;
    socket->interfaces = interfaces;

    return 0;
}

void DeletePlatformWebSocket(PlatformWebSocket** pSocket)
{
    PlatformWebSocket* socket;

    if (pSocket == NULL) {
        return;
    }

    socket = *pSocket;
    if (socket == NULL) {
        return;
    }

    /* 先销毁具体实例（传具体实例，不是抽象包装对象） */
    if (socket->interfaces.deleteInstance != NULL) {
        socket->interfaces.deleteInstance(socket->instance);
        socket->instance = NULL;
    }

    /* 再释放抽象对象 */
    websocketFree(socket);

    /* 最后置空调用方指针，防悬空 */
    *pSocket = NULL;
}

/* =====================================================================
 * 平台具象：Windows（WinHTTP WebSocket）
 * ===================================================================== */
#if defined(PLATFORM_WINDOWS)

#include <windows.h>
#include <winhttp.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>

/* WinHTTP 的「升级为 WebSocket」请求选项。SDK 在部分版本宏组合下不声明该宏，
 * 故此处给出 ABI 等价的兜底定义（值取自 winhttp.h：0x00000070）。 */
#ifndef WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET
#define WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET 0x00000070
#endif

/* WinHTTP WebSocket 的「缓冲区类型」与「关闭状态码」ABI 常量。
 * 数值与 SDK 枚举 WINHTTP_WEB_SOCKET_BUFFER_TYPE / WINHTTP_WEB_SOCKET_CLOSE_STATUS
 * 逐一对应；刻意自定义常量名而**不用 SDK 枚举名**：那些枚举被
 * `#if(_WIN32_WINNT >= 0x0602)` 包住，本编译单元无法假定其可见。
 *   0 = WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE   （FIN 二进制整帧）
 *   1 = WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE  （二进制分片）
 *   2 = WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE     （FIN 文本整帧）
 *   3 = WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE    （文本分片）
 *   4 = WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE            （对端关闭帧） */
#define PLATFORM_WS_BT_BINARY_MESSAGE        0
#define PLATFORM_WS_BT_BINARY_FRAGMENT       1
#define PLATFORM_WS_BT_UTF8_MESSAGE          2
#define PLATFORM_WS_BT_UTF8_FRAGMENT         3
#define PLATFORM_WS_BT_CLOSE                 4
#define PLATFORM_WS_SUCCESS_CLOSE_STATUS  1000   /* WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS */

/* 动态解析出的 WebSocket 函数签名（与 winhttp.h 中的声明逐一对应；
 * 第二参数的枚举在本文件内以 int 传递：MSVC 下枚举底层类型即 int，ABI 完全一致）。 */
typedef HINTERNET (WINAPI *PlatformWsUpgradeFn)(HINTERNET hRequest, DWORD_PTR pContext);
typedef DWORD     (WINAPI *PlatformWsSendFn)(HINTERNET hWebSocket, int bufferType,
                                             void* buffer, DWORD bufferLength);
typedef DWORD     (WINAPI *PlatformWsReceiveFn)(HINTERNET hWebSocket, void* buffer,
                                                DWORD bufferLength, DWORD* bytesRead, int* bufferType);
typedef DWORD     (WINAPI *PlatformWsCloseFn)(HINTERNET hWebSocket, unsigned short closeStatus,
                                               void* reason, DWORD reasonLength);

/* 接收线程写入、pollFrame 所在线程读出的一次「原始接收结果」。
 * 只含 POD（无指针），因此整槽可以按值拷贝、队列可以是一段固定内存 ——
 * 这是「运行期零分配」的前提（槽数组在构造期一次性分配）。 */
typedef struct PlatformWsRecvEvent{
    DWORD         err;                                /* dynReceive 返回码（ERROR_SUCCESS = 拿到数据/控制帧） */
    DWORD         bytesRead;                          /* 本事件字节数（err == ERROR_SUCCESS 时有效） */
    int           bufferType;                         /* PLATFORM_WS_BT_* */
    unsigned char data[PLATFORM_WS_RECV_CHUNK_SIZE];  /* 本次收到的原始字节（仅前 bytesRead 字节有效） */
} PlatformWsRecvEvent;

/* 具体实现实例（刻意定义在此文件作用域内，绝不进头文件，理由见文件顶部说明） */
typedef struct PlatformWebSocketWindows{
    /* ---- 连接配置：构造期已从调用方的 PlatformWebSocketConfig **深拷贝**而来 ---- */
    char host[PLATFORM_WS_HOST_BUF_SIZE];
    int  port;                                    /* 1..65535 */
    int  secure;                                  /* 当前具象恒为 0（wss 未支持，见工厂） */
    char path[PLATFORM_WS_PATH_BUF_SIZE];
    char query[PLATFORM_WS_QUERY_BUF_SIZE];        /* 空串 = 无 query */
    char headerName[PLATFORM_WS_HEADER_NAME_BUF_SIZE];   /* 空串 = 不加额外握手头 */
    char headerValue[PLATFORM_WS_HEADER_VALUE_BUF_SIZE];
    int  connectTimeoutMs;
    int  pollSliceMs;

    /* ---- WinHTTP 句柄 ---- */
    HINTERNET hSession;        /* WinHttpOpen */
    HINTERNET hConnect;        /* WinHttpConnect */
    HINTERNET hRequest;        /* WinHttpOpenRequest；**升级成功后立即关闭并置 NULL** */
    HINTERNET hWebSocket;      /* 握手完成后由 dynUpgrade 返回的 WS 句柄 */
    HMODULE   winhttpModule;   /* 仅当本模块自己 LoadLibraryA 了 winhttp.dll 时非空（需 FreeLibrary） */
    int       connected;       /* 1 = 握手完成，hWebSocket 可用 */

    /* ---- 动态解析的 WebSocket 函数（首次 connect 解析一次并缓存） ---- */
    PlatformWsUpgradeFn dynUpgrade;
    PlatformWsSendFn    dynSend;
    PlatformWsReceiveFn dynReceive;
    PlatformWsCloseFn   dynClose;

    /* ---- 收帧状态机 ---- */
    unsigned char* frameBuf;         /* 当前帧的整帧累积缓冲（按需倍增，连接期内复用） */
    int            frameCap;         /* frameBuf 容量（字节） */
    int            frameLen;         /* 当前帧已累积字节数 */
    int            frameReady;       /* 1 = 当前帧已完整，进入「交付」阶段 */
    int            deliverPos;       /* 当前帧已交付字节数（0..frameLen） */
    PlatformWebSocketFrameType frameType;   /* 当前帧的类型（整帧结束时定稿） */
    int            closeFramePending;/* 1 = 已收到对端 CLOSE 帧，待交付一次 */
    int            pollStopped;      /* 1 = 连接已结束/出错，后续 pollFrame 一律返回 <0 */

    /* ---- 收帧线程 + 内部事件队列（本模块新增，设计见文件顶部《收帧模型》） ----
     * 设计要点：**帧状态机（上面那些成员）仍只在调用方线程读写**，故它们无需加锁；
     * 需要同步的**只有**下面的队列本体。 */
    ThreadObject*        recvThread;      /* 接收线程对象；仅「已连接」期间非空 */
    PlatformMutex*       queueMutex;      /* 保护事件队列（生产者：接收线程；消费者：pollFrame） */
    PlatformWsRecvEvent* queue;           /* 固定深度的事件槽数组（构造期一次性分配，运行期不分配） */
    int                  queueHead;       /* 出队下标；仅 queueMutex 保护下访问 */
    int                  queueTail;       /* 入队下标；仅 queueMutex 保护下访问 */
    int                  queueCount;      /* 当前事件数 0..PLATFORM_WS_QUEUE_DEPTH；仅 mutex 保护下访问 */
    HANDLE               recvExitEvent;   /* manual-reset 事件：接收线程退出时置位，供**带超时的 join** 使用 */
    volatile int         recvStop;        /* 1 = 请求接收线程退出（关闭路径写入） */
    volatile int         recvInCall;      /* 1 = 接收线程正处于（或即将进入）阻塞 Receive */
    volatile int         recvRunning;     /* 1 = 接收线程已进入主循环（诊断用） */
    volatile int         queueDropped;    /* 1 = 曾因队列满丢帧 ⇒ 帧连续性已破坏，必须上报为错误 */
    uint64_t             queueDropLastLogMs;  /* 「队列满丢帧」log_error 的节流时间戳（0 = 未打过） */

    /* ---- 可观测性：当前会话「单次 pollFrame」在 hWebSocket 上的 receive 上界（毫秒） ----
     *   正值 = 本会话**已实际确立**的上界。成立顺序（后写的覆盖先写的）：
     *            · 手段 1（创建 WS 句柄**之前**切 session 超时）成功 ⇒ pollSliceMs；
     *            · 手段 2（对 WS 句柄单独设置）成功          ⇒ pollSliceMs × FACTOR；
     *            · 步骤 12（会话级兜底重设）成功，且此前仍未知 ⇒ pollSliceMs；
     *            · 步骤 12 的「保底值」路径命中              ⇒ 保底值。
     *     -1 = 未知/平台默认（构造期尚未建会，或上述设置全部失败）。
     * 【改造后定位】该字段现在**只作诊断对照**：pollFrame 的「有界」已由接收线程 + 内部队列
     * 保证（真实上界 = pollSliceMs），本字段描述的是 WinHTTP 侧（对 WS 句柄实际无效的）上界，
     * 由接收线程的耗时日志一并打印，便于核对「已知 12018 / 不继承 session 超时」的现象。 */
    int            pollWaitBoundMs;
    int            pollWaitReported; /* 1 = 本会话已就「单次上界」提示过一次（节流：每会话只报一次） */

    /* ---- 可观测性：接收线程「单次（阻塞）Receive 实际耗时」日志的节流时间戳（毫秒，单调时间源） ----
     *   0 = 本会话尚未打过该类日志（首次必定输出）；
     * 非 0 = 上次输出的时刻；仅当与当前时刻之差 >= 节流间隔时才再次输出。
     * 取 0 作哨兵是安全的：本节时间源（Windows 为 QPC / 兜底为 GetTickCount）在实际
     * 运行中不可能取到 0。本字段只由**接收线程**读写，并在构造期与每次启动接收线程时复位。
     * （改造前另有一个「超界 log_error」的节流字段；因 Receive 本就不可设超时、长等待属
     *   预期，该分支与字段已一并移除。） */
    uint64_t       lastRecvOkLogMs;    /* 「接收线程单次 Receive 返回」log_info 的节流时间戳 */

    /* ---- 诊断 ---- */
    DWORD lastError;                        /* 最近一次失败的系统错误码 */
    char  lastErrorText[PLATFORM_WS_ERR_BUF_SIZE];  /* lastError() 返回的只读缓冲 */
} PlatformWebSocketWindows;

static int         wsConnect(void* instance);
static int         wsSendFrame(void* instance, PlatformWebSocketFrameType type, const void* data, int size);
static int         wsPollFrame(void* instance, void* out, int outCap,
                               PlatformWebSocketFrameType* outType, int* outIncomplete, int* hasFrame);
static int         wsClose(void* instance);
static const char* wsLastError(void* instance);
static void        wsDeleteInstance(void* instance);

static int  wsCopyString(char* dst, int dstCap, const char* src, const char* fieldName);
static int  wsValidateAndCopyConfig(PlatformWebSocketWindows* self, const PlatformWebSocketConfig* config);
static int  wsHasCrLf(const char* text);
static int  wsUtf8ToWide(const char* utf8, wchar_t* wide, int wideCapChars);
static int  wsResolveWebSocketFunctions(PlatformWebSocketWindows* self);
static int  wsQueryStatusCode(HINTERNET hRequest);
static void wsCloseAllHandles(PlatformWebSocketWindows* self);
static void wsResetFrameState(PlatformWebSocketWindows* self);
static void wsClearError(PlatformWebSocketWindows* self);
static void wsSetErrorf(PlatformWebSocketWindows* self, const char* fmt, ...);
static int  wsEnsureFrameCapacity(PlatformWebSocketWindows* self, int need);
static int  wsAppendFrameBytes(PlatformWebSocketWindows* self, const unsigned char* data, int len);
static int  wsReceiveTimeoutCode(DWORD err);
/* 收帧线程 + 内部事件队列（定义见「具象实现：收帧」一节） */
static void wsQueueLock(PlatformWebSocketWindows* self);
static void wsQueueUnlock(PlatformWebSocketWindows* self);
static void wsQueueClear(PlatformWebSocketWindows* self);
static int  wsQueuePush(PlatformWebSocketWindows* self, DWORD err, int bufferType,
                        DWORD bytesRead, const unsigned char* data);
static int  wsQueuePop(PlatformWebSocketWindows* self, PlatformWsRecvEvent* out);
static void wsLogQueueDrop(PlatformWebSocketWindows* self);
static int  wsFailOnQueueDrop(PlatformWebSocketWindows* self);
static int  wsApplyRecvResult(PlatformWebSocketWindows* self, DWORD err, int bufferType,
                              DWORD bytesRead, const unsigned char* data);
static int  wsPumpOneEvent(PlatformWebSocketWindows* self, int waitMs);
static int  wsStartReceiveThread(PlatformWebSocketWindows* self);
static void wsStopReceiveThread(PlatformWebSocketWindows* self);
static void wsRecvThreadBody(void* param);
/* 单调时间源 + 耗时计算（仅用于度量单次 Receive 的实际耗时，详见各自定义处注释） */
static uint64_t     wsNowMs(void);
static unsigned int wsElapsedMs(uint64_t startMs, uint64_t endMs);

/* ---------------------------------------------------------------------
 * 诊断 / 状态小工具
 * ------------------------------------------------------------------- */

static void wsClearError(PlatformWebSocketWindows* self)
{
    if (self == NULL) {
        return;
    }
    self->lastError      = 0;
    self->lastErrorText[0] = '\0';
}

/* 把失败原因写入实例内部的只读文本缓冲（lastError() 的返回对象）。
 * 截断是安全的：这是给人看的诊断信息，不参与任何逻辑判断。 */
static void wsSetErrorf(PlatformWebSocketWindows* self, const char* fmt, ...)
{
    va_list ap;

    if (self == NULL || fmt == NULL) {
        return;
    }

    va_start(ap, fmt);
    vsnprintf(self->lastErrorText, sizeof(self->lastErrorText), fmt, ap);
    va_end(ap);
}

/* ---------------------------------------------------------------------
 * 单调时间源（毫秒）——仅用于度量「单次 WinHttpWebSocketReceive 的实际耗时」
 * ---------------------------------------------------------------------
 * 《时间源选择》——复用本层**既有**能力，不新增任何 #include：
 *   1) 首选 platform_timestamp（由 platform.h 聚合包含，本文件已间接可见）：
 *        NewPlatformTimestamp().milliseconds()
 *      Read 到的既有证据：
 *        · platform_websocket.h 第 16 行 #include "platform.h"；
 *        · platform.h 第 147-149 行在 PLATFORM_USE_TIMESTAMP 为真时聚合包含
 *          platform_timestamp.h；后者第 61 行声明 NewPlatformTimestamp()；
 *        · 其 Windows 具象（libs/timestamp/src/timestamp.c:21-26）基于
 *          QueryPerformanceCounter，返回 uint64_t，**单调递增**、不受系统时间
 *          调整（NTP 回拨）影响 —— 正是「度量耗时」所需。
 *   2) 若编译期裁剪了该能力（PLATFORM_USE_TIMESTAMP==0），退回 Win32 GetTickCount()：
 *        · 它由 <windows.h> 提供（本平台分支第 189 行已包含，无需新增 include），
 *          且在**任意** _WIN32_WINNT 组合下都被声明；
 *        · 刻意**不用** GetTickCount64()：它在部分 SDK 的版本宏下不声明，而本工程
 *          禁止在源码里重定义 _WIN32_WINNT（见文件顶部说明），故规避之。
 *
 * 《回绕安全性》：
 *   · platform_timestamp 路径：uint64_t QPC 计数换算，本进程生命周期内不可能回绕；
 *   · GetTickCount 路径：32 位计数约 49.7 天回绕，故**绝不直接比较两个绝对值**，
 *     一律交由 wsElapsedMs() 做「无符号差值 + 回绕补正」；且只在差值落在合理范围
 *     （< 2^32 ms ≈ 49.7 天）内才采信，超出则钳到上限，绝不把异常值当作超长耗时上报。
 * ------------------------------------------------------------------- */
#if PLATFORM_USE_TIMESTAMP
static uint64_t wsNowMs(void)
{
    PlatformTimestampInterface timestamp = NewPlatformTimestamp();

    if (timestamp.milliseconds != NULL) {
        return timestamp.milliseconds();
    }
    /* 理论不可达（能力开启时函数集必已装配）；万一为空仍要保证有时间源，故退回 Win32。 */
    return (uint64_t)GetTickCount();
}
#else
static uint64_t wsNowMs(void)
{
    return (uint64_t)GetTickCount();
}
#endif

/* 计算 startMs -> endMs 的耗时（毫秒），对 32 位回绕时间源安全（见上方《回绕安全性》）。 */
static unsigned int wsElapsedMs(uint64_t startMs, uint64_t endMs)
{
    uint64_t delta;

    if (endMs >= startMs) {
        delta = endMs - startMs;
    } else if (startMs < 0x100000000ULL && endMs < 0x100000000ULL) {
        /* 只有 32 位计数（GetTickCount）才会出现 end < start：计数已回绕，
         * 按 2^32 取模补回真实区间（区间 < 2^31 ms 时结果精确）。 */
        delta = (0x100000000ULL - startMs) + endMs;
    } else {
        /* 理论上不可达（本层时间源单调递增）；保守返回 0，绝不虚报超长耗时。 */
        delta = 0;
    }

    if (delta > 0xFFFFFFFFULL) {
        delta = 0xFFFFFFFFULL;      /* 饱和：把异常值收敛为「约 49.7 天」，不虚报 */
    }

    return (unsigned int)delta;
}

/* 复位收帧状态机（保留已分配的 frameBuf，避免每帧都 malloc/free） */
static void wsResetFrameState(PlatformWebSocketWindows* self)
{
    if (self == NULL) {
        return;
    }
    self->frameLen          = 0;
    self->frameReady        = 0;
    self->deliverPos        = 0;
    self->frameType         = PLATFORM_WEBSOCKET_FRAME_BINARY;
    self->closeFramePending = 0;
    self->pollStopped       = 0;
}

/* 幂等关闭全部 HINTERNET 句柄（任一失败都不得泄漏：只要能关就关，关完置 NULL） */
static void wsCloseAllHandles(PlatformWebSocketWindows* self)
{
    if (self == NULL) {
        return;
    }
    if (self->hWebSocket != NULL) { WinHttpCloseHandle(self->hWebSocket); self->hWebSocket = NULL; }
    if (self->hRequest   != NULL) { WinHttpCloseHandle(self->hRequest);   self->hRequest   = NULL; }
    if (self->hConnect   != NULL) { WinHttpCloseHandle(self->hConnect);   self->hConnect   = NULL; }
    if (self->hSession   != NULL) { WinHttpCloseHandle(self->hSession);   self->hSession   = NULL; }
    self->connected = 0;
}

/* 配置字符串深拷贝。返回 0 成功；<0 = 过长（**绝不截断**：半截 host/path 会连错服务器）。
 * src 为 NULL 或空串 ⇒ 目标置空串（用于 query / 额外握手头等可选字段）。 */
static int wsCopyString(char* dst, int dstCap, const char* src, const char* fieldName)
{
    size_t len;

    if (dst == NULL || dstCap <= 0 || fieldName == NULL) {
        return -1;
    }
    if (src == NULL || src[0] == '\0') {
        dst[0] = '\0';
        return 0;
    }

    len = strlen(src);
    if (len >= (size_t)dstCap) {
        log_error("platform_websocket config: \"%s\" is too long: actual=%u chars limit=%d chars "
                  "(the value is NOT truncated on purpose; shorten it)",
                  fieldName, (unsigned)len, dstCap - 1);
        return -2;
    }

    memcpy(dst, src, len + 1);
    return 0;
}

/* CR/LF 注入防御：host/path/query/额外握手头最终都会进入 HTTP 握手报文，
 * 一旦含 CR/LF 就可以伪造额外请求头（请求走私），故一律拒绝。 */
static int wsHasCrLf(const char* text)
{
    if (text == NULL) {
        return 0;
    }
    return (strchr(text, '\r') != NULL || strchr(text, '\n') != NULL) ? 1 : 0;
}

/* UTF-8 -> UTF-16（WinHTTP 接口是 wchar_t）。返回 0 成功；<0 失败。
 * wideCapChars 为字符数（含结尾 L'\0'），不足则判失败，绝不截断。 */
static int wsUtf8ToWide(const char* utf8, wchar_t* wide, int wideCapChars)
{
    int need;

    if (utf8 == NULL || wide == NULL || wideCapChars <= 0) {
        return -1;
    }

    need = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (need <= 0) {
        return -2;
    }
    if (need > wideCapChars) {
        return -3;
    }
    if (MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, wideCapChars) <= 0) {
        return -4;
    }

    return 0;
}

/* 查询握手响应的 HTTP 状态码（仅用于让升级失败的日志「一条即可定位」） */
static int wsQueryStatusCode(HINTERNET hRequest)
{
    DWORD code = 0;
    DWORD len  = (DWORD)sizeof(code);

    if (hRequest == NULL) {
        return 0;
    }
    if (!WinHttpQueryHeaders(hRequest,
                             WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX,
                             &code, &len, WINHTTP_NO_HEADER_INDEX)) {
        return 0;
    }
    return (int)code;
}

/* 动态解析 WinHTTP WebSocket 函数；解析一次即缓存（重连复用）。返回 0 成功，<0 失败。 */
static int wsResolveWebSocketFunctions(PlatformWebSocketWindows* self)
{
    HMODULE hWinHttp;

    if (self->dynUpgrade != NULL && self->dynSend != NULL &&
        self->dynReceive != NULL && self->dynClose != NULL) {
        return 0;
    }

    /* 链接期已通过 winhttp.lib 导入 winhttp.dll，故 GetModuleHandleA 通常必成功；
     * LoadLibraryA 仅作兜底（例如宿主以延迟加载方式使用）——它返回的模块句柄由
     * 本实例负责 FreeLibrary，绝不泄漏。 */
    hWinHttp = GetModuleHandleA("winhttp.dll");
    if (hWinHttp == NULL) {
        hWinHttp = LoadLibraryA("winhttp.dll");
        if (hWinHttp == NULL) {
            self->lastError = GetLastError();
            wsSetErrorf(self, "LoadLibraryA(\"winhttp.dll\") failed err=%lu", (unsigned long)self->lastError);
            log_error("platform_websocket(WinHTTP) resolve: LoadLibraryA(\"winhttp.dll\") failed err=%lu",
                      (unsigned long)self->lastError);
            return -1;
        }
        self->winhttpModule = hWinHttp;
    }

    /* GetProcAddress 返回 FARPROC（函数指针）；从「一个函数指针」一步强转到「另一个
     * 函数指针」会触发 -Wcast-function-type（GCC 判定两者类型不兼容）。这里按 Win32
     * 动态解析的通行做法：**先经 void* 中转**，把「类型系统无法表达的函数签名桥接」
     * 显式表述出来 —— API 的真实签名由 WinHTTP 文档保证，本层已用 PlatformWs*Fn
     * typedef 逐一对应。此改法的运行期语义与原先的一步强转**完全一致**（同一地址、
     * 同一调用约定），仅是让编译器看到一次「函数指针 <-> 对象指针」的桥接。 */
    self->dynUpgrade = (PlatformWsUpgradeFn)(void*)GetProcAddress(hWinHttp, "WinHttpWebSocketCompleteUpgrade");
    self->dynSend    = (PlatformWsSendFn)(void*)GetProcAddress(hWinHttp, "WinHttpWebSocketSend");
    self->dynReceive = (PlatformWsReceiveFn)(void*)GetProcAddress(hWinHttp, "WinHttpWebSocketReceive");
    self->dynClose   = (PlatformWsCloseFn)(void*)GetProcAddress(hWinHttp, "WinHttpWebSocketClose");

    if (self->dynUpgrade == NULL || self->dynSend == NULL ||
        self->dynReceive == NULL || self->dynClose == NULL) {
        wsSetErrorf(self, "WinHTTP WebSocket API unavailable in winhttp.dll (requires Windows 8 / Server 2012+)");
        log_error("platform_websocket(WinHTTP) resolve: WinHttpWebSocket* not found in winhttp.dll "
                  "(upgrade=%s send=%s receive=%s close=%s); the WebSocket protocol API requires "
                  "Windows 8 / Windows Server 2012 or later",
                  (self->dynUpgrade != NULL) ? "ok" : "missing",
                  (self->dynSend    != NULL) ? "ok" : "missing",
                  (self->dynReceive != NULL) ? "ok" : "missing",
                  (self->dynClose   != NULL) ? "ok" : "missing");
        return -2;
    }

    return 0;
}

/* ---------------------------------------------------------------------
 * 构造期：配置校验 + 深拷贝
 * ------------------------------------------------------------------- */

static int wsValidateAndCopyConfig(PlatformWebSocketWindows* self, const PlatformWebSocketConfig* config)
{
    if (config->host == NULL || config->host[0] == '\0') {
        log_error("platform_websocket config: host is required (null or empty); "
                  "e.g. host=\"127.0.0.1\" (no \"ws://\" prefix, no port). port=%d secure=%d path=\"%s\"",
                  config->port, config->secure, (config->path != NULL) ? config->path : "(null)");
        return -1;
    }
    if (config->port <= 0 || config->port > 65535) {
        log_error("platform_websocket config: port must be in 1..65535 (no default by design); actual=%d host=\"%s\"",
                  config->port, config->host);
        return -2;
    }
    if (config->secure != 0 && config->secure != 1) {
        log_error("platform_websocket config: secure must be 0 (ws) or 1 (wss); actual=%d host=\"%s\"",
                  config->secure, config->host);
        return -3;
    }
    if (config->path == NULL || config->path[0] != '/') {
        log_error("platform_websocket config: path is required and must start with '/'; "
                  "actual=\"%s\" (e.g. \"/v1/stt\")", (config->path != NULL) ? config->path : "(null)");
        return -4;
    }
    if (config->connectTimeoutMs <= 0) {
        log_error("platform_websocket config: connectTimeoutMs must be > 0 (no default by design); actual=%d",
                  config->connectTimeoutMs);
        return -5;
    }
    if (config->pollSliceMs <= 0) {
        log_error("platform_websocket config: pollSliceMs must be > 0 (no default by design); actual=%d",
                  config->pollSliceMs);
        return -6;
    }
    if ((config->headerName == NULL) != (config->headerValue == NULL)) {
        log_error("platform_websocket config: headerName and headerValue must be provided together "
                  "(headerName=\"%s\" headerValue=\"%s\")",
                  (config->headerName != NULL) ? config->headerName : "(null)",
                  (config->headerValue != NULL) ? config->headerValue : "(null)");
        return -7;
    }
    if (config->query != NULL && config->query[0] == '?') {
        log_error("platform_websocket config: query must NOT include the leading '?'; actual=\"%s\" "
                  "(this layer appends it after the path)", config->query);
        return -8;
    }
    if (wsHasCrLf(config->host) || wsHasCrLf(config->path) || wsHasCrLf(config->query) ||
        wsHasCrLf(config->headerName) || wsHasCrLf(config->headerValue)) {
        log_error("platform_websocket config: CR/LF is not allowed in host/path/query/headerName/headerValue "
                  "(handshake header-injection guard)");
        return -9;
    }

    if (wsCopyString(self->host, (int)sizeof(self->host), config->host, "host") != 0) {
        return -10;
    }
    if (wsCopyString(self->path, (int)sizeof(self->path), config->path, "path") != 0) {
        return -11;
    }
    if (wsCopyString(self->query, (int)sizeof(self->query), config->query, "query") != 0) {
        return -12;
    }
    if (wsCopyString(self->headerName, (int)sizeof(self->headerName), config->headerName, "headerName") != 0) {
        return -13;
    }
    if (wsCopyString(self->headerValue, (int)sizeof(self->headerValue), config->headerValue, "headerValue") != 0) {
        return -14;
    }

    self->port             = config->port;
    self->secure           = config->secure;
    self->connectTimeoutMs = config->connectTimeoutMs;
    self->pollSliceMs      = config->pollSliceMs;

    return 0;
}

/* ---------------------------------------------------------------------
 * 具象实现：connect
 * ------------------------------------------------------------------- */

static int wsConnect(void* instance)
{
    PlatformWebSocketWindows* self = (PlatformWebSocketWindows*)instance;
    wchar_t whost[PLATFORM_WS_HOST_BUF_SIZE];
    wchar_t wtarget[PLATFORM_WS_TARGET_BUF_SIZE];
    wchar_t wheader[PLATFORM_WS_HEADER_NAME_BUF_SIZE + PLATFORM_WS_HEADER_VALUE_BUF_SIZE + 4];
    char    target[PLATFORM_WS_TARGET_BUF_SIZE];
    char    header[PLATFORM_WS_HEADER_NAME_BUF_SIZE + PLATFORM_WS_HEADER_VALUE_BUF_SIZE + 4];
    int     targetLen;
    int     headerLen;
    int     rc;

    ASSERT_NOT_NULL(instance, "platform_websocket(WinHTTP) connect: instance is null", -1);

    /* 幂等：已连接再调返回 0 */
    if (self->connected) {
        return 0;
    }

    /* 清空上次的失败信息与收帧状态（重连语义） */
    wsClearError(self);
    wsResetFrameState(self);

    /* 新一轮会话：单次 poll 上界在手段 1/2 或步骤 12 确立前一律视为「未知」；
     * 「单次上界」提示与「单次 Receive 耗时」日志的节流标志也一并复位，
     * 使每个会话各自从零开始节流（首个异常/首个耗时必定留证）。 */
    self->pollWaitBoundMs  = -1;
    self->pollWaitReported = 0;
    self->lastRecvOkLogMs  = 0;

    /* 0) 解析 WebSocket 函数（缓存；失败多为「系统过旧」） */
    rc = wsResolveWebSocketFunctions(self);
    if (rc != 0) {
        return -2;
    }

    /* 1) 组装请求目标：path[?query]，并转宽字符 */
    targetLen = snprintf(target, sizeof(target), "%s%s%s",
                         self->path,
                         (self->query[0] != '\0') ? "?" : "",
                         (self->query[0] != '\0') ? self->query : "");
    /* snprintf 返回「本应写入的长度」：>= 容量即说明被截断，必须失败，
     * 绝不把半截 path 发出去（那会连到错误的端点）。 */
    if (targetLen <= 0 || targetLen >= (int)sizeof(target)) {
        wsSetErrorf(self, "build request target failed (path/query too long)");
        log_error("platform_websocket(WinHTTP) connect: build request target failed: need=%d bytes limit=%d bytes "
                  "path=\"%s\" query=\"%s\"", targetLen, (int)sizeof(target), self->path, self->query);
        return -3;
    }
    if (wsUtf8ToWide(self->host, whost, (int)(sizeof(whost) / sizeof(wchar_t))) != 0) {
        wsSetErrorf(self, "host utf8->utf16 conversion failed");
        log_error("platform_websocket(WinHTTP) connect: host utf8->utf16 failed host=\"%s\"", self->host);
        return -4;
    }
    if (wsUtf8ToWide(target, wtarget, (int)(sizeof(wtarget) / sizeof(wchar_t))) != 0) {
        wsSetErrorf(self, "request target utf8->utf16 conversion failed");
        log_error("platform_websocket(WinHTTP) connect: target utf8->utf16 failed target=\"%s\"", target);
        return -5;
    }

    /* 2) 打开会话。代理沿用与 platform_network 一致的静态代理配置；本层不感知代理策略。 */
    self->hSession = WinHttpOpen(L"translator-platform-websocket/1.0",
                                 WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                 WINHTTP_NO_PROXY_NAME,
                                 WINHTTP_NO_PROXY_BYPASS,
                                 0);
    if (self->hSession == NULL) {
        self->lastError = GetLastError();
        wsSetErrorf(self, "WinHttpOpen failed err=%lu", (unsigned long)self->lastError);
        log_error("platform_websocket(WinHTTP) connect: WinHttpOpen failed err=%lu host=%s port=%d target=%s",
                  (unsigned long)self->lastError, self->host, self->port, target);
        return -6;
    }

    /* 3) 握手阶段整体超时 = connectTimeoutMs（TCP 连接 + 发请求 + 收 101 响应） */
    if (!WinHttpSetTimeouts(self->hSession, self->connectTimeoutMs, self->connectTimeoutMs,
                            self->connectTimeoutMs, self->connectTimeoutMs)) {
        self->lastError = GetLastError();
        wsSetErrorf(self, "WinHttpSetTimeouts(%d ms) failed err=%lu",
                    self->connectTimeoutMs, (unsigned long)self->lastError);
        log_error("platform_websocket(WinHTTP) connect: WinHttpSetTimeouts(%d) failed err=%lu host=%s port=%d",
                  self->connectTimeoutMs, (unsigned long)self->lastError, self->host, self->port);
        wsCloseAllHandles(self);
        return -7;
    }

    /* 4) 定位目标主机 */
    self->hConnect = WinHttpConnect(self->hSession, whost, (INTERNET_PORT)self->port, 0);
    if (self->hConnect == NULL) {
        self->lastError = GetLastError();
        wsSetErrorf(self, "WinHttpConnect failed err=%lu (host=%s port=%d)",
                    (unsigned long)self->lastError, self->host, self->port);
        log_error("platform_websocket(WinHTTP) connect: WinHttpConnect failed err=%lu host=%s port=%d",
                  (unsigned long)self->lastError, self->host, self->port);
        wsCloseAllHandles(self);
        return -8;
    }

    /* 5) 打开握手请求：GET + 无 secure 标志（本具象仅支持明文 ws） */
    self->hRequest = WinHttpOpenRequest(self->hConnect, L"GET", wtarget, NULL,
                                        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                        self->secure ? WINHTTP_FLAG_SECURE : 0);
    if (self->hRequest == NULL) {
        self->lastError = GetLastError();
        wsSetErrorf(self, "WinHttpOpenRequest failed err=%lu", (unsigned long)self->lastError);
        log_error("platform_websocket(WinHTTP) connect: WinHttpOpenRequest failed err=%lu host=%s port=%d target=%s",
                  (unsigned long)self->lastError, self->host, self->port, target);
        wsCloseAllHandles(self);
        return -9;
    }

    /* 6) 声明本次请求要升级为 WebSocket —— 必须在 WinHttpSendRequest 之前设置 */
    if (!WinHttpSetOption(self->hRequest, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, NULL, 0)) {
        self->lastError = GetLastError();
        wsSetErrorf(self, "WinHttpSetOption(WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET) failed err=%lu",
                    (unsigned long)self->lastError);
        log_error("platform_websocket(WinHTTP) connect: WinHttpSetOption(UPGRADE_TO_WEB_SOCKET) failed err=%lu "
                  "host=%s port=%d target=%s", (unsigned long)self->lastError, self->host, self->port, target);
        wsCloseAllHandles(self);
        return -10;
    }

    /* 7) 可选的额外握手请求头（如 Authorization）。用 WinHttpAddRequestHeaders 添加，
     *    与 platform_network 的做法一致；名/值在构造期已通过 CR/LF 注入检查与长度检查。 */
    if (self->headerName[0] != '\0') {
        headerLen = snprintf(header, sizeof(header), "%s: %s", self->headerName, self->headerValue);
        if (headerLen <= 0 || headerLen >= (int)sizeof(header)) {
            wsSetErrorf(self, "build extra handshake header failed (name/value too long)");
            log_error("platform_websocket(WinHTTP) connect: build extra header failed: need=%d bytes limit=%d bytes "
                      "headerName=\"%s\"", headerLen, (int)sizeof(header), self->headerName);
            wsCloseAllHandles(self);
            return -11;
        }
        if (wsUtf8ToWide(header, wheader, (int)(sizeof(wheader) / sizeof(wchar_t))) != 0) {
            wsSetErrorf(self, "extra handshake header utf8->utf16 conversion failed");
            log_error("platform_websocket(WinHTTP) connect: header utf8->utf16 failed headerName=\"%s\"",
                      self->headerName);
            wsCloseAllHandles(self);
            return -12;
        }
        if (!WinHttpAddRequestHeaders(self->hRequest, wheader, (DWORD)-1,
                                      WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE)) {
            self->lastError = GetLastError();
            wsSetErrorf(self, "WinHttpAddRequestHeaders failed err=%lu", (unsigned long)self->lastError);
            log_error("platform_websocket(WinHTTP) connect: WinHttpAddRequestHeaders failed err=%lu headerName=\"%s\"",
                      (unsigned long)self->lastError, self->headerName);
            wsCloseAllHandles(self);
            return -13;
        }
    }

    /* 8) 发送握手请求（无请求体，Content-Length 为 0） */
    if (!WinHttpSendRequest(self->hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, NULL, 0, 0, 0)) {
        self->lastError = GetLastError();
        wsSetErrorf(self, "WinHttpSendRequest failed err=%lu", (unsigned long)self->lastError);
        log_error("platform_websocket(WinHTTP) connect: WinHttpSendRequest failed err=%lu host=%s port=%d target=%s",
                  (unsigned long)self->lastError, self->host, self->port, target);
        wsCloseAllHandles(self);
        return -14;
    }

    /* 9) 收握手响应（受 connectTimeoutMs 约束） */
    if (!WinHttpReceiveResponse(self->hRequest, NULL)) {
        self->lastError = GetLastError();
        wsSetErrorf(self, "WinHttpReceiveResponse failed err=%lu", (unsigned long)self->lastError);
        log_error("platform_websocket(WinHTTP) connect: WinHttpReceiveResponse failed err=%lu host=%s port=%d target=%s",
                  (unsigned long)self->lastError, self->host, self->port, target);
        wsCloseAllHandles(self);
        return -15;
    }

    /* 9.5) ★【手段 1，主修法】在**创建 WS 句柄之前**，把会话超时前移切换为 pollSliceMs。
     *
     * 为什么必须在这里（而不是只在步骤 12 事后设置）：
     *   WinHTTP 的超时是「句柄**创建时**从父句柄**继承**」的。下一步（步骤 10）的
     *   WinHttpWebSocketCompleteUpgrade 会创建 hWebSocket，它继承的是**此刻** session
     *   的超时。若此处仍是握手阶段的 connectTimeoutMs（调用方常配 60000 级），则此后
     *   WinHttpWebSocketReceive 的单次等待上界就不是 pollSliceMs —— 现场表现为
     *   「打完握手帧后长时间静默」（第一次 Receive 迟迟不返回）。而在 WS 句柄**已创建之后**
     *   才去设 session 超时（原步骤 12）属于「事后补救」，未必回溯作用到已创建的 WS 句柄，
     *   故它不能作为唯一的设置点。
     *
     * 此处切换**是安全的**：WinHttpReceiveResponse 已返回成功，握手响应(101)已收完，此后
     * 不再有任何受 connectTimeoutMs 保护的握手期 I/O；而**此后新建**的 WS 句柄将**继承**
     * 这个更小的上界 —— 正是本步的目的。
     *
     * 失败**不致命**：步骤 11.5 会再对 WS 句柄单独设一次、步骤 12 会再做一次兜底重设；
     * 但必须告警，因为它是本会话「单次 I/O 有界」的**第一道**保证。 */
    if (WinHttpSetTimeouts(self->hSession, self->pollSliceMs, self->pollSliceMs,
                           self->pollSliceMs, self->pollSliceMs)) {
        self->pollWaitBoundMs = self->pollSliceMs;
        log_info("platform_websocket(WinHTTP): 握手响应已收完，session 超时已前移切换为 pollSliceMs=%d ms"
                 "（此后创建的 WS 句柄将继承该单次 I/O 上界）host=%s port=%d",
                 self->pollSliceMs, self->host, self->port);
    } else {
        self->lastError = GetLastError();
        /* 此处**不**写 pollWaitBoundMs：本步失败 ⇒ WS 句柄将继承 connectTimeoutMs，
         * 真实上界尚未确知，保持 «-1 = 未知»，交由手段 3 用 connectTimeoutMs 作对照值。 */
        log_error("platform_websocket(WinHTTP): 【手段 1】握手后 session 超时前移失败 err=%lu host=%s port=%d "
                  "—— 即将创建的 WS 句柄会继承握手阶段的 connectTimeoutMs=%d ms，单次 WinHttpWebSocketReceive "
                  "的上界将偏大；将由步骤 11.5（对 WS 句柄单独设置）与步骤 12（兜底重设）补齐。",
                  (unsigned long)self->lastError, self->host, self->port, self->connectTimeoutMs);
    }

    /* 10) 完成升级：拿到 WebSocket 句柄 */
    self->hWebSocket = self->dynUpgrade(self->hRequest, 0);
    if (self->hWebSocket == NULL) {
        int status = wsQueryStatusCode(self->hRequest);

        self->lastError = GetLastError();
        wsSetErrorf(self, "WinHttpWebSocketCompleteUpgrade failed err=%lu httpStatus=%d",
                    (unsigned long)self->lastError, status);
        log_error("platform_websocket(WinHTTP) connect: WinHttpWebSocketCompleteUpgrade failed err=%lu httpStatus=%d "
                  "host=%s port=%d target=%s (expect HTTP 101; 401/403 = credentials, 404 = path, "
                  "426 = server does not support WebSocket)", (unsigned long)self->lastError, status,
                  self->host, self->port, target);
        wsCloseAllHandles(self);
        return -16;
    }

    /* 11) 升级成功后握手用的 request 句柄已无用：**立即**关闭并置 NULL（避免长期持有
     *     可能已被升级过程消费掉的句柄）。若该句柄已被升级消费，WinHttpCloseHandle 只会
     *     返回 FALSE 并置 ERROR_INVALID_HANDLE，既不会崩溃也不影响已取得的 WS 句柄；
     *     紧接着关闭（无其它分配）也把「句柄值被回收复用」的窗口压到最小。 */
    WinHttpCloseHandle(self->hRequest);
    self->hRequest = NULL;

    /* 11.5) ★【手段 2，保险】拿到 hWebSocket 之后，再对**该句柄本身**显式设一次单次 I/O 上界。
     *
     * 为什么要有这一步：手段 1 依赖「WS 句柄在创建时继承父句柄超时」这一行为；本步是它的
     * **独立冗余** —— 即便该继承行为在某个系统版本上不成立，也能把上界钉在句柄上。
     *
     * ⚠️ 但它**不能**作为唯一保证：WinHttpSetTimeouts 的官方文档只说其 hInternet 参数是
     * WinHttpOpen / WinHttpConnect / WinHttpOpenRequest 返回的句柄，**并未承诺接受 WebSocket
     * 句柄**（传错句柄类型会失败并置 ERROR_WINHTTP_INCORRECT_HANDLE_TYPE）。故本步失败属
     * **预期内**：只 log_warn、不致命，且**绝不**把已确立的 pollWaitBoundMs 降级为 -1。 */
    {
        int wsHandleTimeoutMs = self->pollSliceMs;

        /* pollSliceMs × FACTOR 的溢出防护：先判上界再乘，绝不发生有符号溢出。 */
        if (wsHandleTimeoutMs <= INT_MAX / PLATFORM_WS_WS_HANDLE_TIMEOUT_FACTOR) {
            wsHandleTimeoutMs *= PLATFORM_WS_WS_HANDLE_TIMEOUT_FACTOR;
        } else {
            wsHandleTimeoutMs = INT_MAX;
        }

        if (WinHttpSetTimeouts(self->hWebSocket, wsHandleTimeoutMs, wsHandleTimeoutMs,
                               wsHandleTimeoutMs, wsHandleTimeoutMs)) {
            /* 成功 ⇒ WS 句柄的真实上界就是 wsHandleTimeoutMs，故把 pollWaitBoundMs 更新为它：
             * 否则「手段 3」会拿更小的 pollSliceMs 去对照，把 FACTOR 倍之内的合法等待误判为超界。 */
            self->pollWaitBoundMs = wsHandleTimeoutMs;
            log_info("platform_websocket(WinHTTP): 已对 WebSocket 句柄单独设置单次 I/O 上界 = %d ms "
                     "（pollSliceMs=%d × %d）host=%s port=%d",
                     wsHandleTimeoutMs, self->pollSliceMs, PLATFORM_WS_WS_HANDLE_TIMEOUT_FACTOR,
                     self->host, self->port);
        } else {
            DWORD setTimeoutsErr = GetLastError();

            log_warn("platform_websocket(WinHTTP): 对 WebSocket 句柄调用 WinHttpSetTimeouts(%d ms) 失败 err=%lu"
                     " —— API 可能不接受 WebSocket 句柄类型；上界已由手段 1（创建前切换 session 超时）保证，"
                     "本会话单次 I/O 上界仍记为 %d ms（本步失败**不降级**为未知）。host=%s port=%d",
                     wsHandleTimeoutMs, (unsigned long)setTimeoutsErr, self->pollWaitBoundMs,
                     self->host, self->port);
        }
    }

    /* 12) 【兜底重设】把**会话**超时再设一次为 pollSliceMs。
     *     ★注意：这**已不是**唯一的设置点 —— 手段 1（步骤 9.5）已在创建 WS 句柄**之前**
     *     完成切换，那才是让 hWebSocket 继承正确上界的**关键**一步；本步保留是因为它幂等、
     *     无害，并能覆盖其它「会话级」I/O 的等待上界。
     *     失败分两种情形处理：
     *       a) 手段 1 / 手段 2 已确立上界（pollWaitBoundMs > 0）：WS 句柄的上界在创建那一刻
     *          已定型，本步失败只是「重复设置」失败，**不得**把已确知的上界降级或放大；
     *       b) 此前仍未知（pollWaitBoundMs <= 0）：此时才走「保底值」，把『无界/极长』收敛为『有界』
     *          （目的不是恢复节流精度，而是确保有界）。 */
    if (!WinHttpSetTimeouts(self->hSession, self->pollSliceMs, self->pollSliceMs,
                            self->pollSliceMs, self->pollSliceMs)) {
        self->lastError = GetLastError();

        if (self->pollWaitBoundMs > 0) {
            /* 情形 a：已有确立的上界，保留之，仅告警（不降级、不放大）。 */
            log_warn("platform_websocket(WinHTTP): 【兜底重设】会话级 WinHttpSetTimeouts(pollSliceMs=%d ms) 失败 "
                     "err=%lu host=%s port=%d —— 但手段 1 已在创建 WS 句柄之前完成切换，"
                     "本会话单次 I/O 上界仍为 %d ms（不降级）。",
                     self->pollSliceMs, (unsigned long)self->lastError, self->host, self->port,
                     self->pollWaitBoundMs);
        } else {
            /* 情形 b：手段 1 与手段 2 均未确立上界 ⇒ 必须把「无界/极长」收敛为「有界」，并明确告警。 */
            /* 保底值：pollSliceMs 与「保底下限」取较大者 —— 至少 1 秒（保证 > 0），
             * 若 pollSliceMs 本身更大则沿用之（不无谓收紧）。取舍见该宏处注释。 */
            int fallbackMs = (self->pollSliceMs > PLATFORM_WS_POLL_WAIT_FALLBACK_MIN_MS)
                           ? self->pollSliceMs
                           : PLATFORM_WS_POLL_WAIT_FALLBACK_MIN_MS;

            log_error("platform_websocket(WinHTTP): WinHttpSetTimeouts(pollSliceMs=%d ms) FAILED err=%lu "
                      "host=%s port=%d —— 单次 pollFrame 的阻塞上界将退回握手阶段的 receive timeout"
                      "（约 %d ms），轮询节流会失效；故障现场表现为『单次 poll 长时间不返回』。",
                      self->pollSliceMs, (unsigned long)self->lastError, self->host, self->port,
                      self->connectTimeoutMs);

            /* 保底：再试一次「有界但更宽松」的值。目的不是恢复节流，而是把「无界/极长」收敛为「有界」。 */
            if (WinHttpSetTimeouts(self->hSession, fallbackMs, fallbackMs, fallbackMs, fallbackMs)) {
                self->pollWaitBoundMs = fallbackMs;
                log_info("platform_websocket(WinHTTP): 已退化为单次 I/O 上界 = %d ms（pollSliceMs=%d 设置失败，"
                         "保底值保证『有界』，但轮询节流精度下降）", fallbackMs, self->pollSliceMs);
            } else {
                self->lastError = GetLastError();
                self->pollWaitBoundMs = -1;   /* -1 = 未知/平台默认 */
                log_error("platform_websocket(WinHTTP): 保底 WinHttpSetTimeouts(%d ms) 亦失败 err=%lu "
                          "host=%s port=%d —— 本会话的单次 I/O 上界为平台默认值，可能显著偏长；"
                          "手段 1（创建前切换）与手段 2（对 WS 句柄单独设置）此前均已失败，"
                          "调用方的『单次 poll 有界』假设在本会话不成立。",
                          fallbackMs, (unsigned long)self->lastError, self->host, self->port);
            }
        }
    } else if (self->pollWaitBoundMs < 0) {
        /* 会话级设置成功，但此前无任何已确立的上界（手段 1/2 均失败）⇒ 记录会话语义上的上界。
         * 注：此时 WS 句柄已在创建时继承了 connectTimeoutMs，本步只是把「期望上界」记为 pollSliceMs，
         * 便于「手段 3」以 pollSliceMs 为对照，把「实际远大于它」的等待直接定罪。 */
        self->pollWaitBoundMs = self->pollSliceMs;
    }

    /* 13) ★ 启动**专用接收线程**：此后 hWebSocket 的**读**方向只由该线程使用，pollFrame
     *     改为从内部事件队列按 pollSliceMs 有界取出（见文件顶部《收帧模型》）。
     *     放在最后（握手 / 升级 / 超时设置全部完成之后）是为了让线程从一开始就落在最终确立
     *     的配置上；失败即视为连接失败 —— 没有接收线程就没有「有界的 pollFrame」，而底层
     *     Receive 会永久阻塞（这正是本改造要消除的现场故障）。 */
    if (wsStartReceiveThread(self) != 0) {
        wsSetErrorf(self, "start receive thread failed (pollFrame would not be bounded)");
        log_error("platform_websocket(WinHTTP) connect: start receive thread failed host=%s port=%d",
                  self->host, self->port);
        wsStopReceiveThread(self);          /* 幂等：清理可能已部分启动的线程 */
        wsCloseAllHandles(self);
        return -17;
    }

    self->connected = 1;
    wsClearError(self);

    return 0;
}

/* ---------------------------------------------------------------------
 * 具象实现：sendFrame（整帧提交；超过分片上限时自动 MESSAGE + FRAGMENT 切分）
 * ------------------------------------------------------------------- */

/* 把 TEXT 分片切点对齐到 UTF-8 字符边界：若切点正好落在续接字节（0b10xxxxxx）上，
 * 说明切开了多字节字符，向前回退到该字符起点。这样每个分片自身都是合法 UTF-8，
 * 无论驱动侧对 MESSAGE（整帧）还是 FRAGMENT（分片）做 UTF-8 校验都能通过。 */
static int wsAlignUtf8Chunk(const unsigned char* base, int offset, int limit)
{
    int cut = limit;

    /* limit 调用点已保证 offset + limit < 帧总长，故 base[offset + cut] 恒为有效下标 */
    while (cut > 0 && (base[offset + cut] & 0xC0) == 0x80) {
        cut--;
    }
    if (cut == 0) {
        /* 理论不可达（UTF-8 单字符最长 4 字节，而 limit 为 64KiB）；万一到达，
         * 退回 limit，绝不产生 0 长度分片（那会导致死循环）。 */
        cut = limit;
    }

    return cut;
}

/* 线程亲和性（Send 与接收线程的并发）：本函数只在**调用方线程**调用，发送走 WinHTTP 的
 * **写**方向；接收线程只做**读**方向的 WinHttpWebSocketReceive。二者方向不同，不是同向并发
 * （「并发 send/recv 得 1006」的经验来自 Python websocket-client 库，不适用于 WinHTTP）。
 * ⇒ 请勿从其它线程调用本函数，也勿把接收线程改为在调用方线程里 Receive（那正是本改造要
 *    消除的无界阻塞）。完整的论证与最小回退手段见文件顶部《收帧模型》一节。 */
static int wsSendFrame(void* instance, PlatformWebSocketFrameType type, const void* data, int size)
{
    PlatformWebSocketWindows* self = (PlatformWebSocketWindows*)instance;
    const unsigned char*      bytes;
    int                       total;
    int                       sent  = 0;
    int                       first = 1;
    DWORD                     err;

    ASSERT_NOT_NULL(instance, "platform_websocket(WinHTTP) sendFrame: instance is null", -1);

    bytes = (const unsigned char*)data;

    if (!self->connected || self->hWebSocket == NULL || self->dynSend == NULL) {
        wsSetErrorf(self, "sendFrame: no active connection (call connect first)");
        log_error("platform_websocket(WinHTTP) sendFrame: no active connection (call connect first)");
        return -2;
    }
    if (type != PLATFORM_WEBSOCKET_FRAME_TEXT && type != PLATFORM_WEBSOCKET_FRAME_BINARY) {
        /* CLOSE 是「收方向专用」标记；主动关闭请走 close()。 */
        wsSetErrorf(self, "sendFrame: frame type %d is not sendable (TEXT/BINARY only; use close())", (int)type);
        log_error("platform_websocket(WinHTTP) sendFrame: frame type=%d is not sendable "
                  "(only TEXT/BINARY can be sent; use close() to close)", (int)type);
        return -3;
    }

    /* size 归一化：<0 ⇒ 按 NUL 结尾字符串求长（仅 TEXT） */
    if (size < 0) {
        if (type != PLATFORM_WEBSOCKET_FRAME_TEXT) {
            wsSetErrorf(self, "sendFrame: negative size is only allowed for TEXT (NUL-terminated string)");
            log_error("platform_websocket(WinHTTP) sendFrame: size<0 (strlen mode) is only allowed for TEXT; "
                      "type=%d size=%d", (int)type, size);
            return -4;
        }
        if (bytes == NULL) {
            wsSetErrorf(self, "sendFrame: data is null");
            log_error("platform_websocket(WinHTTP) sendFrame: data is null (size<0 = strlen mode requires a string)");
            return -5;
        }
        total = (int)strlen((const char*)bytes);
    } else {
        if (bytes == NULL && size > 0) {
            wsSetErrorf(self, "sendFrame: data is null but size=%d > 0", size);
            log_error("platform_websocket(WinHTTP) sendFrame: data is null but size=%d > 0", size);
            return -6;
        }
        total = size;
    }

    /* 分片循环：首片用 MESSAGE（置 FIN 语义的「消息开始」），其余用 FRAGMENT。
     * 对端（含标准 WS 库）看到的是同一帧，分片对其透明。
     * total == 0 时也要发出一个 0 长度的整帧（first 保证循环至少执行一次）。 */
    while (sent < total || (total == 0 && first)) {
        int chunk = total - sent;
        int bufferType;

        if (chunk > PLATFORM_WS_SEND_CHUNK_SIZE) {
            chunk = PLATFORM_WS_SEND_CHUNK_SIZE;
            if (type == PLATFORM_WEBSOCKET_FRAME_TEXT) {
                chunk = wsAlignUtf8Chunk(bytes, sent, chunk);
            }
        }

        if (type == PLATFORM_WEBSOCKET_FRAME_TEXT) {
            bufferType = first ? PLATFORM_WS_BT_UTF8_MESSAGE : PLATFORM_WS_BT_UTF8_FRAGMENT;
        } else {
            bufferType = first ? PLATFORM_WS_BT_BINARY_MESSAGE : PLATFORM_WS_BT_BINARY_FRAGMENT;
        }

        /* bytes 可能为 NULL（size==0 的空帧）：此时绝不写 NULL+0（那是未定义行为）。 */
        err = self->dynSend(self->hWebSocket, bufferType,
                            (bytes != NULL) ? (void*)(bytes + sent) : NULL, (DWORD)chunk);
        if (err != ERROR_SUCCESS) {
            self->lastError = err;
            wsSetErrorf(self, "WinHttpWebSocketSend failed err=%lu (type=%d sent=%d/%d size=%d)",
                        (unsigned long)err, (int)type, sent, total, size);
            log_error("platform_websocket(WinHTTP) sendFrame: WinHttpWebSocketSend failed err=%lu "
                      "(type=%d bufferType=%d chunk=%d sent=%d/%d size=%d)",
                      (unsigned long)err, (int)type, bufferType, chunk, sent, total, size);
            return -7;
        }

        sent += chunk;
        first = 0;
    }

    return 0;
}

/* ---------------------------------------------------------------------
 * 具象实现：收帧（拼帧 + 交付）
 * ------------------------------------------------------------------- */

/* 确保 frameBuf 至少有 need 字节容量（失败保持原缓冲不动） */
static int wsEnsureFrameCapacity(PlatformWebSocketWindows* self, int need)
{
    unsigned char* newBuf;
    int            newCap;

    if (need <= self->frameCap) {
        return 0;
    }

    newCap = (self->frameCap > 0) ? self->frameCap : PLATFORM_WS_FRAME_INITIAL_CAP;
    while (newCap < need) {
        if (newCap > INT_MAX / 2) {
            newCap = need;
            break;
        }
        newCap *= 2;
    }

    newBuf = (unsigned char*)websocketMalloc((size_t)newCap);
    if (newBuf == NULL) {
        log_error("platform_websocket(WinHTTP) receive: websocketMalloc failed need=%d bytes "
                  "(message is larger than available memory)", need);
        return -1;
    }
    if (self->frameBuf != NULL) {
        websocketFree(self->frameBuf);
    }
    self->frameBuf = newBuf;
    self->frameCap = newCap;

    return 0;
}

/* 把底层刚收到的字节追加进当前帧累积缓冲（整帧拼装） */
static int wsAppendFrameBytes(PlatformWebSocketWindows* self, const unsigned char* data, int len)
{
    int need;

    if (len <= 0) {
        return 0;
    }
    if (self->frameLen > INT_MAX - len) {
        log_error("platform_websocket(WinHTTP) receive: frame size overflow frameLen=%d add=%d",
                  self->frameLen, len);
        return -1;
    }

    need = self->frameLen + len;
    if (wsEnsureFrameCapacity(self, need) != 0) {
        return -2;
    }

    memcpy(self->frameBuf + self->frameLen, data, (size_t)len);
    self->frameLen = need;

    return 0;
}

/* 判定「接收超时」错误码。WinHttpWebSocketReceive 在单次等待超时后返回
 * ERROR_WINHTTP_TIMEOUT；此时连接仍然存活，语义等于「本切片暂无数据」。 */
static int wsReceiveTimeoutCode(DWORD err)
{
    return (err == ERROR_WINHTTP_TIMEOUT) ? 1 : 0;
}

/* =====================================================================
 * 内部事件队列（生产者：接收线程；消费者：pollFrame 所在线程）
 * ===================================================================== */

static void wsQueueLock(PlatformWebSocketWindows* self)
{
    if (self != NULL && self->queueMutex != NULL && self->queueMutex->interfaces.lock != NULL) {
        (void)self->queueMutex->interfaces.lock(self->queueMutex->instance);
    }
}

static void wsQueueUnlock(PlatformWebSocketWindows* self)
{
    if (self != NULL && self->queueMutex != NULL && self->queueMutex->interfaces.unlock != NULL) {
        (void)self->queueMutex->interfaces.unlock(self->queueMutex->instance);
    }
}

/* 清空队列。调用前提：接收线程**尚未启动**或**已确认退出**（否则生产者会与本次清空竞争） */
static void wsQueueClear(PlatformWebSocketWindows* self)
{
    if (self == NULL) {
        return;
    }
    wsQueueLock(self);
    self->queueHead  = 0;
    self->queueTail  = 0;
    self->queueCount = 0;
    wsQueueUnlock(self);
}

/* 「队列满丢帧」的取证与节流告警（**只由接收线程调用**）。
 * 丢帧意味着 WebSocket 帧的**连续性已被破坏**，因此这里只置标志 + 留证；
 * 「上报为错误」由 pollFrame 侧完成（见 wsFailOnQueueDrop），绝不静默继续。 */
static void wsLogQueueDrop(PlatformWebSocketWindows* self)
{
    uint64_t nowMs = wsNowMs();

    if (self->queueDropLastLogMs != 0 &&
        (nowMs - self->queueDropLastLogMs) < (uint64_t)PLATFORM_WS_QUEUE_DROP_LOG_THROTTLE_MS) {
        return;
    }
    self->queueDropLastLogMs = nowMs;

    log_error("platform_websocket(WinHTTP): 接收事件队列已满（深度=%d），已丢弃**最旧**的接收事件 —— "
              "WebSocket 帧的连续性已被破坏，后续 pollFrame 将上报为错误（本连接不可用，需重连）。"
              "host=%s port=%d（通常因调用方长时间未 pollFrame，或对端一次性突发远超队列容量）",
              PLATFORM_WS_QUEUE_DEPTH, self->host, self->port);
}

/* 入队（生产者：接收线程）。返回 0 = 已入库（可能同时丢弃了最旧事件）；-1 = 队列不可用。 */
static int wsQueuePush(PlatformWebSocketWindows* self, DWORD err, int bufferType,
                       DWORD bytesRead, const unsigned char* data)
{
    PlatformWsRecvEvent* slot;
    int                  dropped = 0;

    if (self == NULL || self->queue == NULL || self->queueMutex == NULL) {
        return -1;
    }

    /* 防御：Receive 的 bufferLength 恒为 RECV_CHUNK_SIZE，bytesRead 不应超过它。 */
    if (bytesRead > (DWORD)PLATFORM_WS_RECV_CHUNK_SIZE) {
        bytesRead = (DWORD)PLATFORM_WS_RECV_CHUNK_SIZE;
    }

    wsQueueLock(self);

    if (self->queueCount >= PLATFORM_WS_QUEUE_DEPTH) {
        /* 满：丢**最旧**（head 前移一格）为新事件腾位。
         * 为什么丢最旧而非丢最新：最新事件里可能带 CLOSE / 连接错误，丢掉会造成更大问题。
         * 但丢帧本身已不可接受 —— 无论丢哪端，都必须由 queueDropped 让连接进入「不可用」状态。 */
        self->queueHead = (self->queueHead + 1) % PLATFORM_WS_QUEUE_DEPTH;
        self->queueCount--;
        self->queueDropped = 1;
        dropped = 1;
    }

    slot = &self->queue[self->queueTail];
    slot->err        = err;
    slot->bytesRead  = bytesRead;
    slot->bufferType = bufferType;
    if (bytesRead > 0 && data != NULL) {
        memcpy(slot->data, data, (size_t)bytesRead);
    }

    /* 先填槽、后推进下标与计数：消费者只在 mutex 释放后才能看到该槽，故不会读到半成品。 */
    self->queueTail = (self->queueTail + 1) % PLATFORM_WS_QUEUE_DEPTH;
    self->queueCount++;

    wsQueueUnlock(self);

    if (dropped) {
        wsLogQueueDrop(self);
    }

    return 0;
}

/* 出队（消费者：pollFrame 所在线程）。返回 1 = 取到一个事件；0 = 队列空。 */
static int wsQueuePop(PlatformWebSocketWindows* self, PlatformWsRecvEvent* out)
{
    int got = 0;

    if (self == NULL || out == NULL || self->queue == NULL || self->queueMutex == NULL) {
        return 0;
    }

    wsQueueLock(self);
    if (self->queueCount > 0) {
        *out = self->queue[self->queueHead];     /* 整槽拷贝（含最多 8 KiB 载荷） */
        self->queueHead = (self->queueHead + 1) % PLATFORM_WS_QUEUE_DEPTH;
        self->queueCount--;
        got = 1;
    }
    wsQueueUnlock(self);

    return got;
}

/* =====================================================================
 * 收帧：事件 -> 帧状态机（**只在调用方线程执行**）
 * ===================================================================== */

/* 「队列曾丢帧」的统一上报：帧连续性已破坏 ⇒ 置 pollStopped 并返回 <0，让调用方停止泵送
 * 并重连（**绝不**静默继续把不连续的数据当作正常帧交付）。
 * 幂等：pollStopped 一旦置位，后续 pollFrame 会在步骤②直接返回 -3，不会重复刷日志。 */
static int wsFailOnQueueDrop(PlatformWebSocketWindows* self)
{
    self->pollStopped = 1;
    wsSetErrorf(self, "internal receive queue overflow: at least one received event was dropped, "
                      "so the WebSocket frame stream is no longer contiguous");
    log_error("platform_websocket(WinHTTP) pollFrame: 内部接收队列曾溢出并丢弃过接收事件 —— "
              "WebSocket 帧的连续性已被破坏，本连接不再可用（调用方应停止泵送并重连）。host=%s port=%d",
              self->host, self->port);
    return -2;
}

/* 把一个接收事件并进帧状态机（**只在调用方线程执行**）。本函数体与改造前 wsReceiveOnce 的
 * 后半段逐条一致，只是数据来源由「栈上接收缓冲」换成了「队列事件」—— 状态机本身未改。
 * 返回 >0 = 状态机有推进（帧变完整 或 收到 CLOSE）；
 *      0  = 本事件无进展（分片已累积，或单次等待超时）；
 *      <0 = 连接已结束/出错（调用方应停止泵送）。 */
static int wsApplyRecvResult(PlatformWebSocketWindows* self, DWORD err, int bufferType,
                             DWORD bytesRead, const unsigned char* data)
{
    if (err != ERROR_SUCCESS) {
        if (wsReceiveTimeoutCode(err)) {
            return 0;                       /* 有界等待超时：暂无数据，连接仍活 */
        }
        self->lastError = err;
        self->pollStopped = 1;              /* 连接已断/出错：后续 pollFrame 一律 <0 */
        wsSetErrorf(self, "WinHttpWebSocketReceive failed err=%lu", (unsigned long)err);
        log_error("platform_websocket(WinHTTP) pollFrame: WinHttpWebSocketReceive failed err=%lu "
                  "(connection is treated as closed; the caller should reconnect)", (unsigned long)err);
        return -2;
    }

    /* 对端关闭帧：载荷为空（关闭状态码不走本缓冲，需 QueryCloseStatus 才能取到，
     * 本层不额外解析）。标记待交付一次，供调用方感知「对端已请求关闭」。 */
    if (bufferType == PLATFORM_WS_BT_CLOSE) {
        self->closeFramePending = 1;
        return 1;
    }

    /* 分片或整帧：先并入累积缓冲 */
    if (bytesRead > 0) {
        if (self->frameLen > INT_MAX - (int)bytesRead) {
            self->pollStopped = 1;
            wsSetErrorf(self, "received frame size overflow (frameLen=%d + %lu)", self->frameLen,
                        (unsigned long)bytesRead);
            log_error("platform_websocket(WinHTTP) pollFrame: frame size overflow frameLen=%d + %lu",
                      self->frameLen, (unsigned long)bytesRead);
            return -3;
        }
        if (wsAppendFrameBytes(self, data, (int)bytesRead) != 0) {
            self->pollStopped = 1;
            wsSetErrorf(self, "buffer the received frame failed (%lu bytes, accumulated=%d)",
                        (unsigned long)bytesRead, self->frameLen);
            return -4;
        }
    }

    /* 整帧结束（MESSAGE 类型）⇒ 定稿类型并进入交付阶段；FRAGMENT ⇒ 继续拼帧 */
    if (bufferType == PLATFORM_WS_BT_BINARY_MESSAGE || bufferType == PLATFORM_WS_BT_UTF8_MESSAGE) {
        self->frameType  = (bufferType == PLATFORM_WS_BT_UTF8_MESSAGE) ? PLATFORM_WEBSOCKET_FRAME_TEXT
                                                                       : PLATFORM_WEBSOCKET_FRAME_BINARY;
        self->frameReady = 1;
        return 1;
    }

    return 0;
}

/* 取一个事件推进状态机；队列空时**按 waitMs 有界等待**（这正是 pollSliceMs 的落点）。
 * 返回 >0 = 状态机有推进；0 = 本次（waitMs 内）无事件可取（连接仍活）；<0 = 出错（含丢帧）。 */
static int wsPumpOneEvent(PlatformWebSocketWindows* self, int waitMs)
{
    PlatformWsRecvEvent    recvEvent;
    PlatformSleepInterface sleep;
    uint64_t               t0;
    int                    slice;

    /* 丢帧优先上报：帧连续性已破坏，队列里剩下的事件也不能再当作连续帧累积。 */
    if (self->queueDropped != 0) {
        return wsFailOnQueueDrop(self);
    }

    if (wsQueuePop(self, &recvEvent)) {
        return wsApplyRecvResult(self, recvEvent.err, recvEvent.bufferType,
                                 recvEvent.bytesRead, recvEvent.data);
    }

    if (waitMs <= 0) {
        return 0;
    }

    /* 队列空 ⇒ 有界等待：分片睡眠 + 轮询（platform_sleep 只提供 sleep，没有「可等待事件」
     * 原语；这是依赖最小、语义最直白的等待方式）。分片 = min(waitMs, SLICE_MS)，
     * 故「到点返回」的额外延迟不超过一个分片。 */
    sleep = NewPlatformSleep();
    slice = (waitMs < PLATFORM_WS_QUEUE_POLL_SLICE_MS) ? waitMs : PLATFORM_WS_QUEUE_POLL_SLICE_MS;
    if (slice < 1) {
        slice = 1;
    }

    t0 = wsNowMs();
    for (;;) {
        sleep.sleepMs((uint32_t)slice);

        if (wsQueuePop(self, &recvEvent)) {
            return wsApplyRecvResult(self, recvEvent.err, recvEvent.bufferType,
                                     recvEvent.bytesRead, recvEvent.data);
        }
        if (self->queueDropped != 0) {
            return wsFailOnQueueDrop(self);
        }
        if (self->recvStop != 0) {
            break;                          /* 正在关闭：不再等待，立即返回 0 */
        }
        if ((wsNowMs() - t0) >= (uint64_t)waitMs) {
            break;                          /* 到点：本次无事件 ⇒ 返回 0（连接仍活） */
        }
    }

    return 0;
}

/* =====================================================================
 * 专用接收线程（把不可中断的阻塞 Receive 隔离到独立线程）
 * ===================================================================== */

/* 线程体：只做「阻塞 Receive + 度量 + 入队」，**绝不触碰帧状态机**。
 *   - 线程内只允许：调用 receiveFn、算耗时（诊断）、入队、读写 recvStop/recvInCall/
 *     recvRunning/queueDropped；
 *   - 帧累积 / 分片交付 / lastError / pollStopped 等全部留在调用方线程（wsApplyRecvResult）
 *     ⇒ 那些实例状态无需加锁，把引入新并发 bug 的风险压到最小；
 *   - 句柄在入循环前**拷到局部变量**：此后不再读 self->hWebSocket，故关闭路径把它置 NULL
 *     不会让本线程拿到 NULL 去调用 Receive；
 *   - 退出条件：recvStop 置位，或 receiveFn 返回致命错误（事件已入队，处置在调用方线程）。 */
static void wsRecvThreadBody(void* param)
{
    PlatformWebSocketWindows* self = (PlatformWebSocketWindows*)param;
    HINTERNET                 hSocket;
    PlatformWsReceiveFn       receiveFn;
    unsigned char             chunk[PLATFORM_WS_RECV_CHUNK_SIZE];

    if (self == NULL) {
        return;
    }

    hSocket   = self->hWebSocket;      /* 局部副本：此后关闭路径置 NULL 不影响本线程 */
    receiveFn = self->dynReceive;

    self->recvRunning = 1;

    /* 句柄/函数为空时不进循环（connect 已保证二者非空；此处纯防御，仍会正常置位退出事件）。 */
    while (self->recvStop == 0 && hSocket != NULL && receiveFn != NULL) {
        DWORD        bytesRead  = 0;
        int          bufferType = PLATFORM_WS_BT_BINARY_FRAGMENT;
        DWORD        err;
        uint64_t     startMs;
        uint64_t     endMs;
        unsigned int elapsedMs;

        /* ★【手段 3】（改造后语义）度量**本线程**这一次阻塞 Receive 的实际耗时。
         * 改造后该调用本就不可设超时，长等待（对端久不发言）**属预期**，故这里只打
         * 诊断性的 log_info（节流），不再当作「超时未生效」的错误上报。 */
        startMs = wsNowMs();
        self->recvInCall = 1;          /* 告知关闭路径「线程即将/正在阻塞 Receive」 */
        err = receiveFn(hSocket, chunk, (DWORD)sizeof(chunk), &bytesRead, &bufferType);
        self->recvInCall = 0;
        endMs     = wsNowMs();
        elapsedMs = wsElapsedMs(startMs, endMs);

        if (self->recvStop != 0) {
            break;                     /* 关闭路径已置位：不再入队、不再记日志，立即退出 */
        }

        if (elapsedMs >= PLATFORM_WS_RECV_ELAPSED_LOG_MIN_MS) {
            if (self->lastRecvOkLogMs == 0 ||
                (endMs - self->lastRecvOkLogMs) >= (uint64_t)PLATFORM_WS_RECV_LOG_THROTTLE_MS) {
                self->lastRecvOkLogMs = endMs;
                log_info("platform_websocket(WinHTTP): 接收线程的单次 WinHttpWebSocketReceive 返回"
                         "（本次阻塞等待 %llu ms；该调用不可设置超时，长等待属预期 —— 会话声明的 "
                         "WinHTTP 单次上界=%d ms 仅供对照）err=%lu bytesRead=%lu bufferType=%d host=%s port=%d",
                         (unsigned long long)elapsedMs, self->pollWaitBoundMs,
                         (unsigned long)err, (unsigned long)bytesRead, bufferType,
                         self->host, self->port);
            }
        }

        /* 单次等待超时：理论不可达（WS 句柄不受 WinHttpSetTimeouts 约束）；若真发生，
         * 语义等于「暂无数据」，不入队，直接下一轮。 */
        if (wsReceiveTimeoutCode(err)) {
            continue;
        }

        if (wsQueuePush(self, err, bufferType, bytesRead, chunk) != 0) {
            log_error("platform_websocket(WinHTTP): 接收事件入队失败（内部队列不可用），接收线程退出 "
                      "host=%s port=%d", self->host, self->port);
            break;
        }

        if (err != ERROR_SUCCESS) {
            break;                     /* 致命错误：事件已入队，退出；处置在调用方线程 */
        }
    }

    self->recvRunning = 0;
    if (self->recvExitEvent != NULL) {
        SetEvent(self->recvExitEvent);  /* 通知关闭路径「本线程已退出」—— 供带超时的 join */
    }
}

/* 启动接收线程（connect 成功后调用）。返回 0 = 成功；<0 = 失败。 */
static int wsStartReceiveThread(PlatformWebSocketWindows* self)
{
    PlatformThreadConfig cfg;

    if (self == NULL || self->queue == NULL || self->queueMutex == NULL || self->recvExitEvent == NULL) {
        log_error("platform_websocket(WinHTTP): start receive thread failed: queue subsystem is not "
                  "initialized (host=%s port=%d)",
                  (self != NULL) ? self->host : "(null)", (self != NULL) ? self->port : 0);
        return -1;
    }

    /* 新会话从「空队列、无丢帧、未请求退出」开始 */
    self->recvStop           = 0;
    self->recvInCall         = 0;
    self->recvRunning        = 0;
    self->queueDropped       = 0;
    self->queueDropLastLogMs = 0;
    self->lastRecvOkLogMs    = 0;
    wsQueueClear(self);
    ResetEvent(self->recvExitEvent);

    memset(&cfg, 0, sizeof(cfg));
    cfg.stackSize = 0;      /* 0 = 平台默认 */
    cfg.priority  = 0;
    cfg.name      = NULL;
    cfg.workFunc  = NULL;   /* 线程体与实参由 interfaces.start 注入（见 ThreadObjectWindows::start） */
    cfg.workParam = NULL;

    self->recvThread = NewPlatformThreadObject(&cfg);
    if (self->recvThread == NULL) {
        log_error("platform_websocket(WinHTTP): create receive thread object failed host=%s port=%d",
                  self->host, self->port);
        return -2;
    }

    if (self->recvThread->interfaces.start(self->recvThread->instance, wsRecvThreadBody, self) != 0) {
        log_error("platform_websocket(WinHTTP): start receive thread failed host=%s port=%d",
                  self->host, self->port);
        DeleteThreadObject(self->recvThread);
        self->recvThread = NULL;
        return -3;
    }

    log_info("platform_websocket(WinHTTP): 专用接收线程已启动 —— pollFrame 改为从内部队列"
             "按 pollSliceMs=%d ms 有界取出（队列深度=%d，单事件槽=%d 字节）host=%s port=%d",
             self->pollSliceMs, PLATFORM_WS_QUEUE_DEPTH, PLATFORM_WS_RECV_CHUNK_SIZE,
             self->host, self->port);
    return 0;
}

/* 停止接收线程（幂等）。顺序：置 recvStop → 关 WS 句柄（唯一能中断阻塞 Receive 的手段）
 * → 带超时 join → 清队列。**此后**才允许做既有清理（关闭其余句柄 / 复位帧状态）。 */
static void wsStopReceiveThread(PlatformWebSocketWindows* self)
{
    PlatformSleepInterface     sleep;
    PlatformTimestampInterface ts;
    uint64_t                   t0;
    DWORD                      waitResult;
    int                        timedOut = 0;

    if (self == NULL) {
        return;
    }

    /* ① 置退出标志：接收线程在每次 Receive 返回后都会看到它并跳出循环。 */
    self->recvStop = 1;

    if (self->recvThread == NULL) {
        return;                     /* 从未启动（如 connect 失败路径）：无线程可停 */
    }

    sleep = NewPlatformSleep();
    ts    = NewPlatformTimestamp();

    /* ② 先等接收线程**进入**阻塞 Receive（recvInCall），把「线程恰好在两次调用之间」的窗口
     *    压到最小；取不到也照关（理由见 ③）。 */
    t0 = ts.milliseconds();
    while (self->recvInCall == 0 && (ts.milliseconds() - t0) < PLATFORM_WS_RECV_SETTLE_WAIT_MS) {
        sleep.sleepMs(1);
    }

    /* ③ 关掉 WS 句柄 —— **唯一**能让阻塞在 WinHttpWebSocketReceive 里的线程返回的手段。
     *
     * ⚠️ 权衡（务必知晓）：MS 文档**未承诺**「一个线程 CloseHandle 另一个线程正在使用的
     *    句柄」是安全的。之所以仍然这样做：WinHttpWebSocketReceive **无法设置超时**
     *    （WinHttpSetTimeouts 对 WS 句柄返回 12018；WS 句柄也不继承 session 超时），
     *    不关句柄 ⇒ 接收线程永久阻塞 ⇒ close()/deleteInstance 变成**无界等待**。
     *    前提与约束（三点，缺一不可）：
     *      · 只在**关闭路径**做，且关完立即把 self->hWebSocket 置 NULL；此后本实例与接收
     *        线程都**不再**把该句柄当作有效句柄使用（线程用的是入循环前拷贝的局部副本，
     *        且它每次再次调用 Receive 之前必先检查 recvStop）；
     *      · 关之前先尽可能确认线程正处于阻塞 Receive（步骤 ②）；
     *      · 关之后立即 join（步骤 ④，带超时），超时则强停（步骤 ⑥）。
     *    另注：调用方 wsClose 会**先**尝试用 dynClose 发出关闭帧（该句柄此刻仍有效），
     *    再进入本函数 —— 顺序与理由见 wsClose 内注释。 */
    if (self->hWebSocket != NULL) {
        WinHttpCloseHandle(self->hWebSocket);
        self->hWebSocket = NULL;
    }

    /* ④ join（带超时）：用**事件**等待线程自然退出，而不是轮询 ThreadObject 的 isRun。
     *    原因：isRun 由线程体自行置位，存在「刚创建、尚未被调度」与「已退出」不可区分的
     *    窗口，轮询会误判；而事件只会在线程**确实退出时**被置位，故不会误判。
     *    超时 ⇒ 线程可能仍存活（其 WS 句柄已关闭，此后不得再访问），记录 error 并强停。 */
    if (self->recvExitEvent != NULL) {
        waitResult = WaitForSingleObject(self->recvExitEvent, (DWORD)PLATFORM_WS_RECV_THREAD_JOIN_MS);
        timedOut   = (waitResult != WAIT_OBJECT_0);
        if (timedOut) {
            log_error("platform_websocket(WinHTTP): 等待接收线程退出超时（%d ms，wait=%lu）—— "
                      "该线程可能仍存活（曾进入主循环=%d；其持有的 WS 句柄已被关闭，此后不得再访问"
                      "该句柄）；即将强制终止。host=%s port=%d",
                      PLATFORM_WS_RECV_THREAD_JOIN_MS, (unsigned long)waitResult, self->recvRunning,
                      self->host, self->port);
        }
    } else {
        /* 理论不可达（启动线程前已强制要求事件存在）：兜底退回 isRun 轮询。 */
        t0 = ts.milliseconds();
        while (self->recvThread->interfaces.isRun(self->recvThread->instance) != 0 &&
               (ts.milliseconds() - t0) < PLATFORM_WS_RECV_THREAD_JOIN_MS) {
            sleep.sleepMs(2);
        }
        timedOut = (self->recvThread->interfaces.isRun(self->recvThread->instance) != 0);
    }

    if (!timedOut) {
        /* ⑤ 线程已确实退出；再短暂等 isRun 落零（线程体返回后由 ThreadObject 置位），
         *    避免 DeleteThreadObject 在「已退出但标志未落零」时走 TerminateThread 分支。
         *    这不是正确性所需（线程确已退出），只是让清理路径无歧义、不产生多余强停。 */
        t0 = ts.milliseconds();
        while (self->recvThread->interfaces.isRun(self->recvThread->instance) != 0 &&
               (ts.milliseconds() - t0) < PLATFORM_WS_RECV_SETTLE_WAIT_MS) {
            sleep.sleepMs(1);
        }
    }

    /* ⑥ 销毁线程对象：若线程仍在运行，DeleteThreadObject 内部会 stop()（TerminateThread）
     *    并回收句柄，保证不泄漏（强停是最后手段，正常路径走不到）。 */
    DeleteThreadObject(self->recvThread);
    self->recvThread = NULL;

    /* ⑦ 生产者已不存在 ⇒ 现在清空队列是安全的。 */
    wsQueueClear(self);
    self->recvInCall         = 0;
    self->recvRunning        = 0;
    self->recvStop           = 0;
    self->queueDropped       = 0;
    self->queueDropLastLogMs = 0;
}

static int wsPollFrame(void* instance, void* out, int outCap,
                       PlatformWebSocketFrameType* outType, int* outIncomplete, int* hasFrame)
{
    PlatformWebSocketWindows* self = (PlatformWebSocketWindows*)instance;
    int                       deliverable;
    int                       take;

    ASSERT_NOT_NULL(instance, "platform_websocket(WinHTTP) pollFrame: instance is null", -1);

    /* 出参统一先置「本次无交付」，交付成功后再覆盖（避免调用方读到脏值）；
     * *outType 仅在确实交付到帧时才写（见头文件契约）。 */
    if (hasFrame != NULL) {
        *hasFrame = 0;
    }
    if (outIncomplete != NULL) {
        *outIncomplete = 0;
    }

    if (!self->connected || self->hWebSocket == NULL || self->dynReceive == NULL) {
        wsSetErrorf(self, "pollFrame: no active connection (call connect first)");
        log_error("platform_websocket(WinHTTP) pollFrame: no active connection (call connect first)");
        return -1;
    }
    if (out != NULL && outCap <= 0) {
        wsSetErrorf(self, "pollFrame: outCap<=0 (%d) but out is not null", outCap);
        log_error("platform_websocket(WinHTTP) pollFrame: outCap=%d <= 0 but out is not null", outCap);
        return -2;
    }

    /* ① 待交付的对端 CLOSE 帧：以「零长度帧」交付一次（return 0 + *hasFrame=1），
     *    随后 pollStopped 生效，下一次调用返回 <0，提示调用方停止泵送并自行重连。 */
    if (self->closeFramePending) {
        self->closeFramePending = 0;
        self->pollStopped       = 1;
        if (outType != NULL) {
            *outType = PLATFORM_WEBSOCKET_FRAME_CLOSE;
        }
        if (hasFrame != NULL) {
            *hasFrame = 1;
        }
        if (outIncomplete != NULL) {
            *outIncomplete = 0;
        }
        return 0;
    }

    /* ② 连接已结束/出错：后续调用一律 <0 */
    if (self->pollStopped) {
        return -3;
    }

    /* ③ 拼帧阶段：若当前帧尚未完整，从**内部事件队列**取一个事件推进状态机
     *    （每次 pollFrame 至多取一个事件；队列空时最多等待 pollSliceMs —— 由接收线程 +
     *     内部队列保证**有界**，绝不在此阻塞到「对端发帧为止」）。 */
    if (!self->frameReady) {
        int rc = wsPumpOneEvent(self, self->pollSliceMs);

        if (rc < 0) {
            return -4;                      /* 连接关闭/出错（含「内部队列丢帧」） */
        }
        if (rc == 0) {
            /* 「本次无完整帧」这一分支现在是**有界**到达的（等待上限即 pollSliceMs）。
             * 该提示每会话只报一次（pollWaitReported），避免每 pollSliceMs 一轮的刷屏。 */
            if (!self->pollWaitReported) {
                self->pollWaitReported = 1;
                log_info("platform_websocket(WinHTTP) pollFrame: 本次（%d ms 内）无完整帧可取 —— "
                         "pollFrame 现已由专用接收线程 + 内部队列保证**有界**（等待上限 = pollSliceMs），"
                         "到点即返回 0 且连接仍活。host=%s port=%d",
                         self->pollSliceMs, self->host, self->port);
            }
            return 0;                       /* 本切片无进展：0 = 暂无完整帧，连接仍活 */
        }
        /* 收到 CLOSE：交回上面 ① 的分支逻辑（本轮直接就地交付更省一次调用） */
        if (self->closeFramePending) {
            self->closeFramePending = 0;
            self->pollStopped       = 1;
            if (outType != NULL) {
                *outType = PLATFORM_WEBSOCKET_FRAME_CLOSE;
            }
            if (hasFrame != NULL) {
                *hasFrame = 1;
            }
            return 0;
        }
    }

    /* ④ 交付阶段：把已完整的整帧按 outCap 分片拷贝给调用方（**拷贝语义**） */
    deliverable = self->frameLen - self->deliverPos;

    if (out == NULL) {
        /* 只泵送不交付：数据留在内部缓冲，绝不丢，等调用方带有效 out 再来取 */
        return 0;
    }

    take = (deliverable < outCap) ? deliverable : outCap;
    if (take > 0) {
        memcpy(out, self->frameBuf + self->deliverPos, (size_t)take);
    }
    self->deliverPos += take;

    if (outType != NULL) {
        *outType = self->frameType;
    }
    if (hasFrame != NULL) {
        *hasFrame = 1;
    }

    if (self->deliverPos >= self->frameLen) {
        /* 整帧已交付完毕：复位，准备下一帧。
         * 注：零长度帧在此路径下 take == 0 且 return 0，靠 *hasFrame==1 表达「确实取到一帧」。 */
        if (outIncomplete != NULL) {
            *outIncomplete = 0;
        }
        self->frameReady = 0;
        self->frameLen   = 0;
        self->deliverPos = 0;
    } else {
        if (outIncomplete != NULL) {
            *outIncomplete = 1;             /* 本帧还有剩余，请继续调用 */
        }
    }

    return take;
}

/* ---------------------------------------------------------------------
 * 具象实现：close / lastError / deleteInstance
 * ------------------------------------------------------------------- */

static int wsClose(void* instance)
{
    PlatformWebSocketWindows* self = (PlatformWebSocketWindows*)instance;
    int                       result = 0;

    ASSERT_NOT_NULL(instance, "platform_websocket(WinHTTP) close: instance is null", -1);

    /* 发送关闭帧。失败（例如对端已先行关闭）只记录，**不阻止**资源回收：
     * 「关不掉」绝不能变成「句柄泄漏」。
     * 幂等：已关闭（无 WS 句柄）再调不会重复发送。
     *
     * 【与接收线程的顺序】必须**先**发关闭帧、**后**停接收线程：
     *   wsStopReceiveThread() 会关掉 WS 句柄（那是中断阻塞 Receive 的唯一手段），此后就再也
     *   没机会发关闭帧了，故只能在此刻发。
     *   此刻接收线程可能正阻塞在 Receive 上，二者方向不同 —— 本函数走**写/关闭**方向，
     *   接收线程只做**读**方向；本层在会话内不再对同一句柄做其它调用。
     *   ⚠️遗留不确定性：若某版本 WinHTTP 的 WinHttpWebSocketClose 内部**还会等待对端的关闭帧**
     *   （即内部也做一次读），则它与接收线程的 Receive 存在争用可能。当前没有找到禁止该并发
     *   的文档证据，故按上述顺序实现；**一旦现场观察到 close() 长时间不返回**，最小处置是
     *   改为「先关句柄（跳过关闭帧）再清理」——即以「不发送关闭帧」换取 close() 必定有界。 */
    if (self->hWebSocket != NULL && self->dynClose != NULL) {
        DWORD err = self->dynClose(self->hWebSocket, (unsigned short)PLATFORM_WS_SUCCESS_CLOSE_STATUS, NULL, 0);

        if (err != ERROR_SUCCESS) {
            self->lastError = err;
            wsSetErrorf(self, "WinHttpWebSocketClose failed err=%lu (peer may have closed first; "
                              "all handles are reclaimed anyway)", (unsigned long)err);
            log_error("platform_websocket(WinHTTP) close: WinHttpWebSocketClose failed err=%lu "
                      "(peer may have closed first; handles are reclaimed anyway)", (unsigned long)err);
            result = -2;
        }
    }

    /* 停止接收线程：置 recvStop → 关 WS 句柄（中断阻塞的 Receive）→ 带超时 join → 清队列。
     * 必须**先于**其余资源回收：否则接收线程还可能在用这些资源（甚至已释放的内存）。 */
    wsStopReceiveThread(self);

    wsCloseAllHandles(self);
    wsResetFrameState(self);

    return result;
}

static const char* wsLastError(void* instance)
{
    PlatformWebSocketWindows* self = (PlatformWebSocketWindows*)instance;

    if (instance == NULL) {
        return "";                          /* 契约：绝不返回 NULL */
    }

    /* 返回实例内部的只读缓冲：无错误时为初始的零填充空串 */
    return self->lastErrorText;
}

static void wsDeleteInstance(void* instance)
{
    PlatformWebSocketWindows* self;

    if (instance == NULL) {
        return;
    }
    self = (PlatformWebSocketWindows*)instance;

    /* 复用 close 的回收路径：发送关闭帧 + 关闭全部 HINTERNET 句柄（幂等） */
    (void)wsClose(instance);

    if (self->frameBuf != NULL) {
        websocketFree(self->frameBuf);
        self->frameBuf = NULL;
    }
    self->frameCap = 0;
    self->frameLen = 0;

    /* 接收线程的配套资源：此时 wsClose 已停止并 join 了接收线程（生产者不存在），
     * 故可安全释放队列槽数组 / 互斥量 / 退出事件。 */
    if (self->queue != NULL) {
        websocketFree(self->queue);
        self->queue = NULL;
    }
    if (self->queueMutex != NULL) {
        DeletePlatformMutex(&self->queueMutex);
    }
    if (self->recvExitEvent != NULL) {
        CloseHandle(self->recvExitEvent);
        self->recvExitEvent = NULL;
    }

    /* 若本实例自己 LoadLibraryA 了 winhttp.dll，此处成对释放 */
    if (self->winhttpModule != NULL) {
        FreeLibrary(self->winhttpModule);
        self->winhttpModule = NULL;
    }

    /* 销毁具体实例（抽象包装器由 DeletePlatformWebSocket 释放） */
    websocketFree(self);
}

/* 组装 Windows 具象虚表 */
static PlatformWebSocketInterface windowsInterface(void)
{
    PlatformWebSocketInterface interfaces;

    interfaces.connect        = wsConnect;
    interfaces.sendFrame      = wsSendFrame;
    interfaces.pollFrame      = wsPollFrame;
    interfaces.close          = wsClose;
    interfaces.lastError      = wsLastError;
    interfaces.deleteInstance = wsDeleteInstance;

    return interfaces;
}

/* Windows 具体构造：装配具体实例 + 调抽象构造完成包装（禁止手工赋值 instance/interfaces） */
static PlatformWebSocket* NewPlatformWebSocketWindows(const PlatformWebSocketConfig* config)
{
    PlatformWebSocketWindows* self;
    PlatformWebSocket*        socket;

    self = (PlatformWebSocketWindows*)websocketMalloc(sizeof(PlatformWebSocketWindows));
    ASSERT_NOT_NULL(self, "NewPlatformWebSocketWindows: websocketMalloc failed", NULL);
    memset(self, 0, sizeof(PlatformWebSocketWindows));

    /* 构造期显式初始化「单次 pollFrame 阻塞上界」：此时尚未建立任何会话，语义为「未知/平台默认」，
     * 与 -1 一致（memset 给出的 0 不属于该字段的取值域，故显式覆盖）；节流标志与「单次 Receive
     * 实际耗时」日志的时间戳一并归零（0 = 尚未打过该类日志，首次必定输出）。 */
    self->pollWaitBoundMs   = -1;
    self->pollWaitReported  = 0;
    self->lastRecvOkLogMs   = 0;

    /* 配置校验 + 深拷贝（调用方的 config 及其字符串可在本函数返回后被释放） */
    if (wsValidateAndCopyConfig(self, config) != 0) {
        websocketFree(self);
        self = NULL;
        return NULL;
    }

    /* 接收线程的内部事件队列：**构造期一次性分配**（运行期零分配）—— 固定深度的事件槽
     * 数组，每槽自带 8 KiB 载荷缓冲；深度取舍见 PLATFORM_WS_QUEUE_DEPTH 处注释。 */
    self->queue = (PlatformWsRecvEvent*)websocketMalloc(
                      sizeof(PlatformWsRecvEvent) * (size_t)PLATFORM_WS_QUEUE_DEPTH);
    if (self->queue == NULL) {
        log_error("NewPlatformWebSocketWindows: alloc receive queue failed (%d slots x %u bytes)",
                  PLATFORM_WS_QUEUE_DEPTH, (unsigned)sizeof(PlatformWsRecvEvent));
        websocketFree(self);
        self = NULL;
        return NULL;
    }
    memset(self->queue, 0, sizeof(PlatformWsRecvEvent) * (size_t)PLATFORM_WS_QUEUE_DEPTH);
    self->queueHead  = 0;
    self->queueTail  = 0;
    self->queueCount = 0;

    /* 保护事件队列的互斥量（复用 platform_sync 能力；生产者/消费者各一个线程） */
    self->queueMutex = NewPlatformMutexForPlatform();
    if (self->queueMutex == NULL) {
        log_error("NewPlatformWebSocketWindows: create queue mutex failed");
        websocketFree(self->queue);
        self->queue = NULL;
        websocketFree(self);
        self = NULL;
        return NULL;
    }

    /* 接收线程的「退出事件」：manual-reset + 初始未置位；每次启动接收线程前 ResetEvent，
     * 线程退出时 SetEvent —— 这是本层能做**带超时的 join** 的关键（ThreadObject 的 isRun 标志
     * 存在「刚创建尚未调度」与「已退出」不可区分的窗口，见 wsStopReceiveThread 内说明）。 */
    self->recvExitEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (self->recvExitEvent == NULL) {
        DWORD createErr = GetLastError();

        log_error("NewPlatformWebSocketWindows: CreateEvent (receive-thread exit) failed err=%lu",
                  (unsigned long)createErr);
        DeletePlatformMutex(&self->queueMutex);
        websocketFree(self->queue);
        self->queue = NULL;
        websocketFree(self);
        self = NULL;
        return NULL;
    }

    socket = NewPlatformWebSocket(self, windowsInterface());
    if (socket == NULL) {
        CloseHandle(self->recvExitEvent);
        self->recvExitEvent = NULL;
        DeletePlatformMutex(&self->queueMutex);
        websocketFree(self->queue);
        self->queue = NULL;
        websocketFree(self);
        self = NULL;
        return socket;
    }

    return socket;
}

/* 平台中立工厂：Windows -> WinHTTP WebSocket 具象 */
PlatformWebSocket* NewPlatformWebSocketNative(const PlatformWebSocketConfig* config)
{
    if (config == NULL) {
        log_error("NewPlatformWebSocketNative: config is null (host/port/path/connectTimeoutMs/pollSliceMs "
                  "are all required; this layer ships zero defaults on purpose)");
        return NULL;
    }

    if (config->secure) {
        /* 必须显式失败：静默降级为明文会把凭据/音频暴露在网络上（安全缺陷，不是容错）。 */
        log_error("NewPlatformWebSocketNative: secure=1 (wss/TLS) is NOT supported by the current WinHTTP "
                  "concrete implementation (plaintext ws only); refusing to silently downgrade to ws. "
                  "host=\"%s\" port=%d path=\"%s\"",
                  (config->host != NULL) ? config->host : "(null)", config->port,
                  (config->path != NULL) ? config->path : "(null)");
        return NULL;
    }

    return NewPlatformWebSocketWindows(config);
}

/* =====================================================================
 * 其它平台（Linux / macOS / FreeRTOS）：暂无可移植具象
 * ===================================================================== */
#else

/* 保证任何平台都能链接通过：工厂返回 NULL（并 log_error）。
 * 抽象构造 / DeletePlatformWebSocket 仍然可用（上层可自行注入自备的具体实例）。 */
PlatformWebSocket* NewPlatformWebSocketNative(const PlatformWebSocketConfig* config)
{
    log_error("NewPlatformWebSocketNative: websocket client not available on this platform "
              "(host=\"%s\" port=%d secure=%d; only Windows/WinHTTP has a concrete implementation)",
              (config != NULL && config->host != NULL) ? config->host : "(null)",
              (config != NULL) ? config->port : 0,
              (config != NULL) ? config->secure : 0);
    return NULL;
}

#endif /* 平台互斥分支 */

#endif /* PLATFORM_USE_WEBSOCKET */
