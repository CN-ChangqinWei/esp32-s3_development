#ifndef _PLATFORM_WEBSOCKET_H
#define _PLATFORM_WEBSOCKET_H

#include <stddef.h>
#include <stdint.h>

/* 本头会被 C++ 编译单元（Qt 具象层 / demo）包含，故自带 extern "C" 包裹，
   且把本项目 C 头 platform.h 的包含置于包裹**之内**：这样无论本头与平台聚合头
   谁先被包含，platform.h 及其内部的全部 platform_*.h 都以 C 链接展开，
   消除「谁先包含」对语言链接的隐式耦合。platform.h 自身亦带包裹，
   此处出现的「extern "C" 嵌套」在 C++ 中是合法且幂等的。 */
#ifdef __cplusplus
extern "C" {
#endif

#include "platform.h"

/* =====================================================================
 * platform_websocket —— 跨平台「WebSocket 客户端」能力门面（可轮询对象式）
 *
 * 职责：
 *   为「非 UI 层不得使用 Qt」提供纯 C 的 WebSocket 客户端能力。调用方
 *   （如 core 的流式语音识别 / 流式 LLM 具象，运行在应用层工作线程里）采用
 *   「建连 → 泵送取帧 → 判连接存活」的轮询模型，故本能力**必须可轮询**：
 *   connect 只负责建连 + 完成握手（Upgrade）即返回；
 *   pollFrame 每次最多取走**一个**已到达的底层接收事件（不负责「一次调用收齐整帧」，
 *   整帧由实现内部缓冲累积、必要时分片交付）。在实现内部的**专用接收线程 + 事件队列**
 *   支撑下，该调用现在是**真正有界**的：等待上限即 pollSliceMs，到点仍无事件即返回 0。
 *   之所以必须用接收线程隔离：底层 WinHttpWebSocketReceive **不可设置超时**
 *   （细则与两条已取证的事实见下方 pollFrame 槽的契约注记），否则一次「预期为空」的
 *   轮询就会永久卡死。
 *
 * 平台差异（由 platform_websocket.c 的互斥分支选择，头文件不感知平台）：
 *   Windows : WinHTTP WebSocket 具象（WebSocket API 经 GetProcAddress 动态解析）
 *   Linux / macOS / FreeRTOS : 暂无可移植具象，NewPlatformWebSocketNative() 返回 NULL
 *                              （并 log_error，保证任何平台都能链接通过）
 *
 * ---------------------------------------------------------------------
 * 能力范围（现状，务必先读）：
 * ---------------------------------------------------------------------
 *   1) **仅支持明文 ws（secure == 0）**。当前 Windows 具象未实现 TLS 客户端
 *      证书校验，故 secure == 1（wss）时 NewPlatformWebSocketNative()
 *      **必定 log_error 并返回 NULL**；**绝不静默降级为明文**（静默降级会把
 *      凭据与音频暴露在网络上，属安全缺陷而非容错）。
 *   2) 本层**零默认值**：host / port / path / connectTimeoutMs / pollSliceMs
 *      全部必须由调用方经 PlatformWebSocketConfig 显式注入，库内不内置任何
 *      默认主机、默认端口、默认路径、默认请求头。
 *   3) 本层只做「帧」的收发（文本 / 二进制），**不做**子协议协商、不做自动
 *      重连、不做心跳保活、不做 ping/pong 主动发送 —— 这些是调用方的策略。
 *   4) 零 Qt：本头与实现均不引入任何 Qt 头。
 *   5) **【有界性 · 已由专用接收线程 + 内部队列保证】**Windows 具象的底层
 *      WinHttpWebSocketReceive **不可设置超时**，两条已取证的事实（保留作设计依据）：
 *      WinHttpSetTimeouts **不接受 WebSocket 句柄**（对 hWebSocket 调用返回 FALSE 且
 *      GetLastError()==12018，ERROR_WINHTTP_INCORRECT_HANDLE_TYPE）；且
 *      WinHttpWebSocketCompleteUpgrade 新建的 WS 句柄**不继承** session 超时
 *      （即便在 CompleteUpgrade 之前把 session 超时设得很小，实测仍无效）。
 *      本层因此用**专用接收线程**承载该阻塞：接收线程只做 Receive + 入队，`pollFrame`
 *      改为从内部队列**按 pollSliceMs 有界取出** ⇒ `pollSliceMs` **现在确实是**单次调用的
 *      等待上限（到点无事件即返回 0，连接仍活）。细则见下方 pollFrame 槽的契约注记。
 *      仍需注意：**不可**用「一次预期为空的 poll」去判断「对端已无数据 / 已收齐」——
 *      轮询只能说明「此刻队列为空」；收齐判断请由协议层（消息长度 / 结束标记）或调用方
 *      的超时策略完成（本项目 vosk 识别单元曾因依赖底层超时而永久卡死）。
 *
 * ---------------------------------------------------------------------
 * 轮询语义（contract，调用方务必遵守）：
 * ---------------------------------------------------------------------
 *   connect   —— 建连 + 握手（含首轮 Upgrade）。**幂等**：已连接再调返回 0。
 *                0 = 成功；<0 = 失败（失败原因见 lastError()）。
 *   sendFrame —— 整帧提交，实现内部负责分片与掩码（单帧超过内部发送分片上限时
 *                自动以「首片 MESSAGE + 后续 FRAGMENT」的方式切分，对端无感）。
 *                size < 0 表示「按 NUL 结尾字符串求长度」，**仅 TEXT 允许**。
 *                0 = 成功；<0 = 失败。CLOSE 类型**不可**用于发送。
 *   pollFrame —— 泵送取帧。【设计意图】单次调用最多取走**一个**已到达的接收事件，
 *                并由**专用接收线程 + 内部队列**保证**有界**：等待上限即 `pollSliceMs`，
 *                到点无事件即返回 0（连接仍活）。底层 WinHttpWebSocketReceive 仍
 *                **不可设置超时**（12018 / 不继承 session 超时），接收线程正是为隔离
 *                该阻塞而设 —— 细则见下方 pollFrame 槽的契约注记。
 *                返回 >0 = 本次交付的字节数（若 *outIncomplete 为 0，则本次已把
 *                          该帧完整给出；若为 1，则本帧还有剩余，需继续调用）；
 *                返回  0 = 本次没有完整帧可取（**连接仍活**，稍后再泵）；
 *                          【有界性】本分支**必定**在 `pollSliceMs` 量级返回 —— 由专用接收
 *                          线程 + 内部队列保证，不再像改造前那样阻塞到对端发帧为止；
 *                          特例：零长度帧（含对端 CLOSE 帧）也返回 0，但会置
 *                          *hasFrame = 1、*outIncomplete = 0、*outType = 帧类型，
 *                          调用方**必须以 hasFrame 判定「是否取到帧」**；
 *                          （零长度帧 / 对端 CLOSE 到达时同样立即返回 0 并置 hasFrame=1）
 *                返回 <0 = 连接已关闭或出错（调用方应停止泵送并自行重连）。
 *                【单帧大于 outCap】⇒ 允许分多次交付：本次填满 outCap 返回 outCap，
 *                并把 *outIncomplete 置 1；调用方应**连续调用**以取走同一帧的剩余部分。
 *                【缓冲归属】本接口是**拷贝语义**：帧数据由实现拷入 out[0,outCap)，
 *                调用方无需 free、也不得假设帧数据指向内部缓冲。
 *                【出参】*outType / *outIncomplete / *hasFrame 三者均可为 NULL（则不回写）；
 *                *outType 仅在 *hasFrame 被置 1 时才被写入。
 *                【out == NULL 或 outCap <= 0】视为「只泵送不交付」：数据留在实现内部
 *                缓冲（**不丢**），返回 0；调用方随后用有效 out 重新调用即可取走。
 *   close     —— 请求关闭（**幂等**）：发送关闭帧并回收全部底层资源；关闭后仍可
 *                再次 connect()（配置在构造期已深拷进实例）。0 = 成功；<0 = 失败
 *                （即便关闭帧发送失败，底层句柄也一定被回收，绝不泄漏）。
 *   lastError —— 最近一次失败的可读原因。返回**指向实例内部只读缓冲**的 NUL 结尾
 *                字符串；无错误时返回空串 ""（**绝不返回 NULL**）；
 *                调用方只读、**不得 free、不得改写**。
 *   deleteInstance —— 关闭连接并释放实例（幂等）。
 *
 * ---------------------------------------------------------------------
 * 重要偏离说明（有意为之，勿“顺手修正”）：
 * ---------------------------------------------------------------------
 *   本头**只暴露抽象接口**，具体实现结构体（Windows 的 HINTERNET 会话句柄、
 *   帧累积缓冲、动态解析出的 WebSocket 函数指针等）一律定义在
 *   platform_websocket.c 的文件作用域内，刻意**不放进头文件**。原因：本头会被
 *   C++ 的 Qt 编译单元间接包含，一旦在此引入 <windows.h> / <winhttp.h>，其内的
 *   COM 宏 `#define interface struct` 以及 IN / OUT / TRUE / FALSE / min / max
 *   等宏会污染整个工程，并可能与本头中形如 `interfaces` 的字段名冲突，导致
 *   C++ 编译期出现难以定位的错误。故此处对「具体实现结构体放头文件」的规范做
 *   **有意偏离**，特此注明。
 * ===================================================================== */

/* 帧类型（收发共用） */
typedef enum PlatformWebSocketFrameType{
    PLATFORM_WEBSOCKET_FRAME_TEXT   = 0,   /* UTF-8 文本帧 */
    PLATFORM_WEBSOCKET_FRAME_BINARY = 1,   /* 二进制帧 */
    PLATFORM_WEBSOCKET_FRAME_CLOSE  = 2    /* 对端已发关闭帧（收方向专用） */
} PlatformWebSocketFrameType;

/* 连接配置（全部信息外部注入；库内零默认值） */
typedef struct PlatformWebSocketConfig{
    const char* host;         /* 必填：主机名或 IP（不要带 ws:// 前缀、不要带端口） */
    int         port;         /* 必填：> 0 */
    int         secure;       /* 1 = wss（TLS），0 = ws（明文）。Windows 具象当前仅支持 0；见 .c 说明 */
    const char* path;         /* 必填：请求路径，以 '/' 开头（如 "/v1/stt"） */
    const char* query;        /* 可选：附加查询串（不含 '?'），如 "model=whisper&language=zh"；NULL/空 = 无 */
    const char* headerName;   /* 可选：额外握手请求头名（如 "Authorization"）；NULL/空 = 不加 */
    const char* headerValue;  /* 可选：该请求头的值（如 "Bearer xxx"） */
    int         connectTimeoutMs;  /* 必填：> 0，握手阶段的整体超时（毫秒） */
    int         pollSliceMs;       /* 必填：> 0。单次收发泵送的**有界**等待上限（毫秒）。pollFrame 已由**专用接收线程 + 内部队列**保证真正有界：单次调用至多等待本值即返回 0（连接仍活）。底层 WinHttpWebSocketReceive 不可设置超时（12018 / 不继承 session 超时），接收线程正是为隔离该阻塞而设，详见本头 pollFrame 槽的契约注记 */
} PlatformWebSocketConfig;

typedef struct PlatformWebSocketInterface{
    /* 建立连接（含握手；幂等：已连接再调返回 0）。0 = 成功；<0 = 失败（日志给出原因）。 */
    int  (*connect)(void* instance);
    /* 发送一帧（整帧提交，实现内部负责分片/掩码）。
     * size < 0 时按 NUL 结尾字符串求长度（仅 TEXT 允许）。0 = 成功；<0 = 失败。 */
    int  (*sendFrame)(void* instance, PlatformWebSocketFrameType type, const void* data, int size);
    /* 泵送一帧：把收到的**一帧**写入 out[0,outCap)。
     * 【有界性】单次调用**有界**：最多等待 pollSliceMs（从实现内部的接收线程 + 事件队列中
     * 取出一个已到达的接收事件）；到点无事件即返回 0（连接仍活）。见下方注记。
     * 返回 >0 = 该帧字节数（*outIncomplete 置 0 表示整帧已完整给出）；
     *      0  = 本次没有完整帧可取（连接仍活，调用方稍后再泵）；
     *      <0 = 连接已关闭或出错（调用方应停止泵送并自行重连）；
     * 【若单帧大于 outCap】⇒ 允许分多次交付：本次填满 outCap 返回 outCap，并把 *outIncomplete 置 1，
     *      调用方应继续调用以取走剩余部分（同一帧剩余部分）。*outIncomplete 可为 NULL。
     * 【*outType】写本次交付帧的类型；【*hasFrame】置 1 表示本次确实取到了一帧（可能是某帧的第一片）。
     * 三个出参均可为 NULL（则不回写）。
     * 【实现差异 · 重要】Windows 具象（WinHTTP WebSocket）的底层 WinHttpWebSocketReceive
     *   **不可设置超时**，两条已取证的事实（保留作设计依据）：
     *   - WinHttpSetTimeouts **不接受 WebSocket 句柄**：对 hWebSocket 调用返回 FALSE 且
     *     GetLastError() == 12018（ERROR_WINHTTP_INCORRECT_HANDLE_TYPE）；
     *   - WinHttpWebSocketCompleteUpgrade 新建的 WS 句柄**也不继承 session 的超时**
     *     （即使在 CompleteUpgrade 之前就把 session 超时设得很小，实测仍无效）。
     *   ⟹ 【本层的对策与现状】已用**专用接收线程 + 内部队列**隔离该阻塞：接收线程只做
     *     Receive + 入队，本函数从队列**按 pollSliceMs 有界取出**。因此 `pollSliceMs`
     *     **现在确实是**单次调用的等待上限，调用方**可以依赖它**获得「单次有界」。
     * 仍需注意：**不要**用「一次预期为空的 poll 来确认已收齐（连接仍活）」来判断「对端已无
     *   数据」—— 轮询只能说明「此刻队列为空」，并不表示对端不会再发；收齐判断请由协议层
     *   （消息长度 / 结束标记）或调用方自己的超时策略完成（本项目 vosk 识别单元曾因依赖
     *   底层超时而永久卡死）。
     * 【队列满的语义】接收线程在队列满时会丢弃**最旧**的接收事件并置「已丢帧」标志；随后
     *   本函数会把它**上报为错误（返回 <0）** —— 丢帧意味着 WebSocket 帧的连续性已被破坏，
     *   继续当作正常数据交付会导致静默的协议错乱，故绝不静默继续。 */
    int  (*pollFrame)(void* instance, void* out, int outCap,
                      PlatformWebSocketFrameType* outType, int* outIncomplete, int* hasFrame);
    /* 请求关闭（幂等；发送关闭帧并回收底层资源）。0 = 成功；<0 = 失败。 */
    int  (*close)(void* instance);
    /* 最近一次失败的可读原因：必须返回**指向实例内部只读缓冲**的 NUL 结尾字符串；
     * 无错误时返回空串 ""；**不得返回 NULL**；调用方只读、不得 free。 */
    const char* (*lastError)(void* instance);
    void (*deleteInstance)(void* instance);   /* 关闭连接并释放实例（幂等） */
} PlatformWebSocketInterface;

typedef struct PlatformWebSocket{
    void*                      instance;      /* 指向具体实现实例（多态核心） */
    PlatformWebSocketInterface interfaces;    /* 虚表，字段名固定为 interfaces */
} PlatformWebSocket;

/* 抽象构造：参数表固定，勿改（只做「分配抽象对象 + 存 instance + 存虚表」三件事） */
PlatformWebSocket* NewPlatformWebSocket(void* instance, PlatformWebSocketInterface interfaces);
int                InitPlatformWebSocket(PlatformWebSocket* socket, void* instance,
                                         PlatformWebSocketInterface interfaces);
void               DeletePlatformWebSocket(PlatformWebSocket** pSocket);

#if PLATFORM_USE_WEBSOCKET
/* 平台中立具象工厂：Windows → WinHTTP WebSocket；其余平台返回 NULL（并 log_error，保证可链接）。
 * 连接信息（host/port/secure/path/query/额外握手头/超时）全部由 config 注入，
 * 本层不带任何默认值；config 为 NULL、字段非法、或 secure==1（wss 未支持）时
 * 一律 log_error 并返回 NULL。config 中的字符串会被**深拷贝**进实例，
 * 调用方可在本函数返回后立即释放自己持有的 config 及其字符串。 */
PlatformWebSocket* NewPlatformWebSocketNative(const PlatformWebSocketConfig* config);
#endif

#ifdef __cplusplus
}
#endif

#endif /* _PLATFORM_WEBSOCKET_H */
