#ifndef _PLATFORM_NETWORK_H
#define _PLATFORM_NETWORK_H

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
 * platform_network —— 跨平台「HTTP 客户端」能力门面（可轮询对象式）
 *
 * 职责：
 *   为「非 UI 层不得使用 Qt」提供纯 C 的 HTTP(S) 访问能力。调用方
 *   （如 core 的 ollama 具象，运行在应用层工作线程里）采用
 *   「发起 → 泵送取增量 → 判完成」的轮询模型，因此本能力**必须可轮询**：
 *   begin 只负责建立连接、发出请求并等到「响应头」到达即返回；
 *   poll 每次只做一次有界的网络 I/O 并取回本次可得的响应体增量，
 *   绝不「一次调用阻塞到响应结束」。
 *
 * 平台差异（由 platform_network.c 的互斥分支选择，头文件不感知平台）：
 *   Windows : WinHTTP 具象（winhttp.lib / winhttp.dll）
 *   Linux   : BSD socket 具象（仅支持明文 http）
 *   macOS   : BSD socket 具象（仅支持明文 http）
 *   FreeRTOS: 无网络具象，NewPlatformNetworkNative() 返回 NULL（保证可链接）
 *
 * 语义约定：
 *   begin   —— 隐式结束上一路未完成会话（幂等）；0 成功进入接收阶段，<0 失败。
 *   poll    —— out == NULL 只泵送不取数据（数据必须留在底层缓冲，绝不能丢）；
 *              out != NULL 且 outCap <= 0 视为非法入参，返回 <0；
 *              返回 >0 本次取出的字节数；0 暂无数据（仍在进行）；<0 出错。
 *              *isDone = 1 表示响应已结束（含对端正常关闭）。
 *   statusCode —— HTTP 状态码；未收到响应头返回 0；无会话返回 <0。
 *   deleteInstance —— 释放会话与实例（幂等）。
 *
 * 重要偏离说明（有意为之，勿“顺手修正”）：
 *   本头**只暴露抽象接口**，具体实现结构体（Windows 的 HINTERNET 会话、
 *   POSIX 的 socket fd 等）一律定义在 platform_network.c 的文件作用域内，
 *   刻意**不放进头文件**。原因：本头会被 C++ 的 Qt demo 间接包含，一旦在此
 *   引入 <windows.h> / <winsock.h> 等平台头，其内的 COM 宏
 *   `#define interface struct` 以及 IN / OUT / TRUE / FALSE / min / max 等
 *   宏会污染整个工程，并可能与本头中形如 `interfaces` 的字段名冲突，
 *   导致 C++ 编译期出现难以定位的错误。故此处对「具体实现结构体放头文件」
 *   的规范做**有意偏离**，特此注明。
 * ===================================================================== */

/* 单次 HTTP(S) 请求的全部入参；本层不带任何默认域名/端口/路径/凭据，
 * 请求级信息全部由此结构注入。 */
typedef struct PlatformNetworkRequest{
    const char* url;               /* 必填：http(s)://host[:port]/path（含 query） */
    const char* contentType;       /* NULL = 不带 Content-Type */
    const void* body;              /* 请求体，可为 NULL */
    int         bodyLen;           /* 请求体长度；body 为 NULL 或 bodyLen<=0 表示无请求体 */
    const char* authUser;          /* NULL = 不启用鉴权 */
    const char* authPassword;      /* NULL 视为空串（等价 curl -u user: 语义） */
    int         connectTimeoutMs;  /* 建立连接/等响应头超时；<=0 = 平台默认 */
    int         pollSliceMs;       /* 单次 poll 的有界等待上限；<=0 = 内部常量 20ms */
} PlatformNetworkRequest;

typedef struct PlatformNetworkInterface{
    /* 发起请求（隐式结束上一路未完成会话，幂等）；0 成功进入接收阶段，<0 失败 */
    int  (*begin)(void* instance, const PlatformNetworkRequest* request);
    /* 泵送一次网络 I/O 并取增量响应体：
     *   out == NULL  只泵送不取数据（数据必须留在底层缓冲，绝不能丢）
     *   outCap <= 0 且 out != NULL  入参非法，返回 <0
     *   返回 >0 本次取出的字节数；0 暂无数据（仍在进行）；<0 出错（此后调用方应结束会话）
     *   *isDone = 1 表示响应已结束（含对端正常关闭） */
    int  (*poll)(void* instance, void* out, int outCap, int* isDone);
    /* HTTP 状态码；尚未收到响应头返回 0；无会话返回 <0 */
    int  (*statusCode)(void* instance);
    /* 释放会话与实例（幂等） */
    void (*deleteInstance)(void* instance);
} PlatformNetworkInterface;

typedef struct PlatformNetwork{
    void*                    instance;      /* 指向具体实现实例 */
    PlatformNetworkInterface interfaces;    /* 虚表，字段名固定为 interfaces */
} PlatformNetwork;

/* 抽象构造：参数表固定，勿改（只做「分配抽象对象 + 存 instance + 存虚表」三件事） */
PlatformNetwork* NewPlatformNetwork(void* instance, PlatformNetworkInterface interfaces);
int              InitPlatformNetwork(PlatformNetwork* network, void* instance, PlatformNetworkInterface interfaces);
void             DeletePlatformNetwork(PlatformNetwork** pNetwork);

#if PLATFORM_USE_NETWORK
/* 平台中立工厂：内部按当前平台装配具象并返回抽象对象。请求级信息全部由
 * PlatformNetworkRequest 注入，本层不带任何默认域名/端口/路径/凭据。 */
PlatformNetwork* NewPlatformNetworkNative(void);
#endif /* PLATFORM_USE_NETWORK */

#ifdef __cplusplus
}
#endif

#endif /* _PLATFORM_NETWORK_H */
