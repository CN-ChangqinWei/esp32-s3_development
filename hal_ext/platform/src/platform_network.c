#include "platform_network.h"
#include "assert_marco.h"
#include <string.h>
#include <stddef.h>

/* =====================================================================
 * platform_network —— 跨平台「HTTP 客户端」能力实现（可轮询对象式）
 *
 * 结构（自顶向下）：
 *   1) 能力开关 PLATFORM_USE_NETWORK：关闭时不生成任何函数；
 *   2) 抽象层（与平台无关，能力开启即无条件生成）：
 *        NewPlatformNetwork / InitPlatformNetwork / DeletePlatformNetwork；
 *   3) 平台无关工具（仅 Windows / POSIX 具象使用）：URL 解析、Base64、Basic 鉴权头；
 *   4) 平台互斥分支（同一编译单元只命中一个）：
 *        PLATFORM_WINDOWS                 -> WinHTTP 具象 + NewPlatformNetworkNative
 *        PLATFORM_LINUX / PLATFORM_MACOS  -> BSD socket 具象 + NewPlatformNetworkNative
 *        #else（FreeRTOS 等）             -> NewPlatformNetworkNative 返回 NULL
 *
 * 内存：整个模块的堆分配/释放统一走同模块能力 NewPlatformMemory()
 *       （见 platform_memory.h）；若编译期关闭了内存能力（PLATFORM_USE_MEMORY==0），
 *       为不与本文件的自依赖冲突，退回 <stdlib.h> 的 malloc/free，并在此注明原因。
 *
 * 平台隔离：具体实现结构体（PlatformNetworkWindows / PlatformNetworkPosix）定义在
 *       本 .c 的文件作用域内，绝不写进头文件；同时**严禁**把 <windows.h> 等平台头
 *       放进 platform_network.h —— 该头会被 C++ 的 Qt demo 间接包含，一旦引入
 *       <windows.h>，其 COM 头里的 `#define interface struct` 与 IN/OUT/TRUE/FALSE
 *       等宏会污染整个工程。这是对「具体实现结构体放头文件」规范的**有意偏离**。
 * ===================================================================== */

#if !PLATFORM_USE_NETWORK

/* 能力关闭：不生成任何函数。
 * platform_network.h 中的 typedef 仍然存在（本编译单元非空），无需额外占位。 */

#else /* PLATFORM_USE_NETWORK */

/* ---------------------------------------------------------------------
 * 本模块内部常量
 * ------------------------------------------------------------------- */
#define NETWORK_DEFAULT_POLL_SLICE_MS       20     /* poll 单次有界等待默认值 */
#define NETWORK_DEFAULT_CONNECT_TIMEOUT_MS  30000  /* 建立连接/等响应头默认超时 */
#define NETWORK_HOST_BUF_SIZE               256
#define NETWORK_PATH_BUF_SIZE               2048

/* ---------------------------------------------------------------------
 * 内存：统一走同模块能力 NewPlatformMemory()
 * ------------------------------------------------------------------- */
#if PLATFORM_USE_MEMORY

static void* networkMalloc(size_t size)
{
    PlatformMemoryInterface mem = NewPlatformMemory();
    return mem.malloc(size);
}

static void networkFree(void* ptr)
{
    PlatformMemoryInterface mem = NewPlatformMemory();
    mem.free(ptr);
}

#else /* PLATFORM_USE_MEMORY == 0 */

/* 内存能力被裁剪时 NewPlatformMemory 不可用。本模块必须能分配抽象对象与具体
 * 实例，故按规范允许的例外退回标准库 malloc/free（仅此一处，且为唯一退路）。 */
#include <stdlib.h>

static void* networkMalloc(size_t size)
{
    return malloc(size);
}

static void networkFree(void* ptr)
{
    free(ptr);
}

#endif /* PLATFORM_USE_MEMORY */

/* =====================================================================
 * 抽象层（与平台无关，能力开启即无条件生成）
 * ===================================================================== */

PlatformNetwork* NewPlatformNetwork(void* instance, PlatformNetworkInterface interfaces)
{
    PlatformNetwork* network;

    ASSERT_NOT_NULL(instance, "NewPlatformNetwork: instance is null", NULL);

    network = (PlatformNetwork*)networkMalloc(sizeof(PlatformNetwork));
    ASSERT_NOT_NULL(network, "NewPlatformNetwork: networkMalloc failed", NULL);

    /* 交由 Init 完成装配（只设置 instance/interfaces，禁止手工赋值） */
    if (InitPlatformNetwork(network, instance, interfaces) != 0) {
        networkFree(network);
        network = NULL;
        return network;
    }

    return network;
}

int InitPlatformNetwork(PlatformNetwork* network, void* instance, PlatformNetworkInterface interfaces)
{
    ASSERT_NOT_NULL(network, "InitPlatformNetwork: network is null", -1);
    ASSERT_NOT_NULL(instance, "InitPlatformNetwork: instance is null", -2);

    network->instance   = instance;
    network->interfaces = interfaces;

    return 0;
}

void DeletePlatformNetwork(PlatformNetwork** pNetwork)
{
    PlatformNetwork* network;

    if (pNetwork == NULL) {
        return;
    }

    network = *pNetwork;
    if (network == NULL) {
        return;
    }

    /* 先销毁具体实例（传具体实例，不是抽象包装对象） */
    if (network->interfaces.deleteInstance != NULL) {
        network->interfaces.deleteInstance(network->instance);
        network->instance = NULL;
    }

    /* 再释放抽象对象 */
    networkFree(network);

    /* 最后置空调用方指针，防悬空 */
    *pNetwork = NULL;
}

/* =====================================================================
 * 平台无关工具（仅 Windows / POSIX 具象编译；FreeRTOS 分支不编译，避免未用告警）
 * ===================================================================== */
#if defined(PLATFORM_WINDOWS) || defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)

/* URL 解析结果（与平台无关的纯数据） */
typedef struct NetworkParsedUrl{
    int  isHttps;                              /* 1 = https，0 = http */
    int  port;                                 /* 端口；缺省时 https=443 / http=80 */
    char host[NETWORK_HOST_BUF_SIZE];
    char path[NETWORK_PATH_BUF_SIZE];          /* 含 query */
} NetworkParsedUrl;

/* 解析 http(s)://host[:port]/path（含 query）。不引入任何第三方库。
 * 返回 0 成功；<0 失败：
 *   -1 入参非法；-2 scheme 非法（缺 scheme 或非 http/https）；
 *   -3 host 为空 / 过长；-4 端口非法；-5 path 过长。
 * 每个失败分支的日志都做到「单条即可定位并自纠」：写明合法取值范围、
 * 容量常量与实际值，并保留原始 url 文本（与既有行为一致）。 */
static int parseUrl(const char* url, NetworkParsedUrl* out)
{
    const char*  cursor;
    const char*  hostStart;
    const char*  hostEnd;
    size_t       len;

    if (url == NULL || out == NULL) {
        log_error("parseUrl: url is null or out is null (invalid argument)");
        return -1;
    }

    memset(out, 0, sizeof(NetworkParsedUrl));

    if (strncmp(url, "https://", 8) == 0) {
        out->isHttps = 1;
        out->port    = 443;
        cursor       = url + 8;
    } else if (strncmp(url, "http://", 7) == 0) {
        out->isHttps = 0;
        out->port    = 80;
        cursor       = url + 7;
    } else {
        /* 本层保持严格：不做「无 scheme 时按 http 猜测」的容错（猜 scheme 是调用方
         * 输入边界的职责）。日志必须让用户一眼看懂怎么改：明确指出「必须带 scheme」、
         * 只有 http/https 两种，并给出正确写法示例。 */
        log_error("parseUrl: unsupported scheme (only http/https): url=\"%s\" -> url must start with "
                  "\"http://\" or \"https://\" (scheme is missing or not supported); "
                  "correct example: http://127.0.0.1:11434/api/generate", url);
        return -2;
    }

    /* host：直到 '/' 或 ':' 或 '?' 或字符串结束 */
    hostStart = cursor;
    hostEnd   = hostStart;
    while (*hostEnd != '\0' && *hostEnd != '/' && *hostEnd != ':' && *hostEnd != '?') {
        hostEnd++;
    }
    len = (size_t)(hostEnd - hostStart);
    if (len == 0) {
        log_error("parseUrl: host is empty: url=\"%s\" -> expect http(s)://<host>[:port][/path]; "
                  "correct example: http://127.0.0.1:11434/api/generate", url);
        return -3;
    }
    if (len >= sizeof(out->host)) {
        /* host 容量上界 = NETWORK_HOST_BUF_SIZE 字节（含结尾 '\0'），故可用字符数为 -1 */
        log_error("parseUrl: host is too long: actual=%u chars limit=NETWORK_HOST_BUF_SIZE-1(%d chars); "
                  "url=\"%s\"", (unsigned)len, (int)sizeof(out->host) - 1, url);
        return -3;
    }
    memcpy(out->host, hostStart, len);
    out->host[len] = '\0';

    /* port：可缺省；显式给出时必须是 1..65535 的十进制整数 */
    if (*hostEnd == ':') {
        const char* portStart = hostEnd + 1;
        const char* scan      = portStart;
        int         fragLen;
        long        portVal   = 0;

        /* 逐位累加，一旦超过 65535 就钳制为 65536：
         *   1) 避免 portVal 因超长数字串溢出（有符号溢出是未定义行为）；
         *   2) 保证「超长端口串」必然被判非法，绝不会因回绕而误判成某个合法端口。 */
        while (*scan >= '0' && *scan <= '9') {
            if (portVal <= 65535) {
                portVal = portVal * 10 + (*scan - '0');
                if (portVal > 65535) {
                    portVal = 65536;
                }
            }
            scan++;
        }

        fragLen = (int)(scan - portStart);
        if (fragLen == 0 || portVal <= 0 || portVal > 65535) {
            int shown = (fragLen > 32) ? 32 : fragLen;   /* 只打印片段前 32 字符，避免日志爆炸 */

            log_error("parseUrl: invalid port: legal range is 1..65535; actual fragment=\"%.*s\"(%d chars%s); "
                      "url=\"%s\"", shown, portStart, fragLen, (shown < fragLen) ? ", truncated" : "", url);
            return -4;
        }
        out->port = (int)portVal;
        hostEnd   = scan;
    }

    /* path（含 query）：以 host[:port] 之后的第一个 '/'（或 '?'）起算 */
    if (*hostEnd == '\0') {
        out->path[0] = '/';
        out->path[1] = '\0';
    } else {
        const char* pathStart = hostEnd;
        size_t      pathLen   = strlen(pathStart);
        size_t      need      = (*pathStart == '?') ? (pathLen + 2)   /* 补前导 '/' + 结尾 '\0' */
                                                    : (pathLen + 1);  /* 结尾 '\0' */

        if (need > sizeof(out->path)) {
            log_error("parseUrl: path too long: actual pathLen=%u chars need=%u bytes(incl. '\\0') "
                      "limit=NETWORK_PATH_BUF_SIZE(%u bytes); url=\"%s\"",
                      (unsigned)pathLen, (unsigned)need, (unsigned)sizeof(out->path), url);
            return -5;
        }
        if (*pathStart == '?') {
            out->path[0] = '/';
            memcpy(out->path + 1, pathStart, pathLen + 1);
        } else {
            memcpy(out->path, pathStart, pathLen + 1);
        }
    }

    return 0;
}

static const char networkBase64Table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* 标准 Base64 编码（无换行）。返回写入字符数；<0 失败（入参非法或缓冲不足）。 */
static int base64Encode(const unsigned char* data, int len, char* out, int outCap)
{
    int i;
    int written = 0;

    if (data == NULL || out == NULL || len < 0) {
        return -1;
    }
    /* 每 3 字节 -> 4 字符，另需结尾 '\0' */
    if (outCap < ((len + 2) / 3) * 4 + 1) {
        return -2;
    }

    for (i = 0; i < len; i += 3) {
        unsigned int b0 = data[i];
        unsigned int b1 = (i + 1 < len) ? data[i + 1] : 0;
        unsigned int b2 = (i + 2 < len) ? data[i + 2] : 0;

        out[written++] = networkBase64Table[b0 >> 2];
        out[written++] = networkBase64Table[((b0 & 0x03U) << 4) | (b1 >> 4)];
        out[written++] = (i + 1 < len) ? networkBase64Table[((b1 & 0x0FU) << 2) | (b2 >> 6)] : '=';
        out[written++] = (i + 2 < len) ? networkBase64Table[b2 & 0x3FU] : '=';
    }
    out[written] = '\0';

    return written;
}

/* 组装 "Authorization: Basic <base64(user:password)>"（authPassword==NULL 视为空串，
 * 等价 curl -u user: 语义；不使用“等 401 挑战”的鉴权方式）。返回 0 成功，<0 失败。 */
static int buildBasicAuthHeader(const char* user, const char* password, char* out, int outCap)
{
    char plain[512];
    char encoded[704];
    int  len;
    int  written;

    if (user == NULL || out == NULL || outCap <= 0) {
        return -1;
    }

    len = snprintf(plain, sizeof(plain), "%s:%s", user, (password != NULL) ? password : "");
    if (len <= 0 || (size_t)len >= sizeof(plain)) {
        return -2;
    }

    if (base64Encode((const unsigned char*)plain, len, encoded, (int)sizeof(encoded)) < 0) {
        return -3;
    }

    /* snprintf 返回「本应写入的长度」，>= outCap 即说明已被截断：必须判失败而不是把
     * 半截鉴权头发出去（静默截断会让服务端返回 401 且原因不可见）。 */
    written = snprintf(out, (size_t)outCap, "Authorization: Basic %s", encoded);
    if (written <= 0 || written >= outCap) {
        return -4;
    }

    return 0;
}

#endif /* PLATFORM_WINDOWS || PLATFORM_LINUX || PLATFORM_MACOS */

/* =====================================================================
 * 平台具象：Windows（WinHTTP）
 * ===================================================================== */
#if defined(PLATFORM_WINDOWS)

#include <windows.h>
#include <winhttp.h>
#include <stdio.h>

/* 具体实现实例（刻意定义在此文件作用域内，绝不进头文件，理由见文件顶部说明） */
typedef struct PlatformNetworkWindows{
    HINTERNET hSession;     /* WinHttpOpen 句柄 */
    HINTERNET hConnect;     /* WinHttpConnect 句柄 */
    HINTERNET hRequest;     /* WinHttpOpenRequest 句柄 */
    DWORD     lastError;    /* 最近一次失败的错误码（诊断用） */
} PlatformNetworkWindows;

static int  begin(void* instance, const PlatformNetworkRequest* request);
static int  poll(void* instance, void* out, int outCap, int* isDone);
static int  statusCode(void* instance);
static void deleteInstance(void* instance);
static void closeSession(PlatformNetworkWindows* self);
static int  utf8ToWide(const char* utf8, wchar_t* wide, int wideCapChars);
static int  addHeaderUtf8(HINTERNET hRequest, const char* headerUtf8);

/* begin 失败日志的统一上下文后缀（parseUrl 成功之后的任何失败都带上它）：
 * 一条日志即含 host/port/path/isHttps/url，调用方无需回看上一行就能定位
 * 「是哪个地址的哪一步」失败。用法：
 *   log_error("... err=%lu" NETWORK_WIN_BEGIN_CTX_FMT,
 *             (unsigned long)code, NETWORK_WIN_BEGIN_CTX_ARG(parsed, request->url));  */
#define NETWORK_WIN_BEGIN_CTX_FMT  " host=%s port=%d path=%s isHttps=%d url=%s"
#define NETWORK_WIN_BEGIN_CTX_ARG(p, reqUrl) \
    ((p).host), ((p).port), ((p).path), ((p).isHttps), (reqUrl)

/* 幂等关闭全部句柄并复位会话状态 */
static void closeSession(PlatformNetworkWindows* self)
{
    if (self == NULL) {
        return;
    }
    if (self->hRequest != NULL) { WinHttpCloseHandle(self->hRequest); self->hRequest = NULL; }
    if (self->hConnect != NULL) { WinHttpCloseHandle(self->hConnect); self->hConnect = NULL; }
    if (self->hSession != NULL) { WinHttpCloseHandle(self->hSession); self->hSession = NULL; }
    self->lastError = 0;
}

/* UTF-8 -> UTF-16（WinHTTP 接口是 wchar_t）。返回 0 成功；<0 失败。
 * wideCapChars 为字符数（含结尾 L'\0'），不足则判失败，绝不截断。 */
static int utf8ToWide(const char* utf8, wchar_t* wide, int wideCapChars)
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

/* 以 UTF-8 文本添加一条请求头（内部转宽字符后交给 WinHttpAddRequestHeaders）。
 * WinHttpAddRequestHeaders 可用 UTF-16，但请求头本身通常为 ASCII/UTF-8。 */
static int addHeaderUtf8(HINTERNET hRequest, const char* headerUtf8)
{
    wchar_t wheader[1024];

    if (hRequest == NULL || headerUtf8 == NULL) {
        return -1;
    }
    if (utf8ToWide(headerUtf8, wheader, (int)(sizeof(wheader) / sizeof(wchar_t))) != 0) {
        return -2;
    }
    if (!WinHttpAddRequestHeaders(hRequest, wheader, (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD)) {
        return -3;
    }

    return 0;
}

/* 发起请求：建连 -> 发请求 -> 等到响应头。隐式结束上一路会话（幂等）。 */
static int begin(void* instance, const PlatformNetworkRequest* request)
{
    PlatformNetworkWindows* self = (PlatformNetworkWindows*)instance;
    NetworkParsedUrl        parsed;
    /* UTF-8 -> UTF-16 宽字符缓冲的容量依据（边界核查结论：恰好够用，无静默截断）：
     *   UTF-8 的 1 字节最多对应 1 个 wchar_t（纯 ASCII 时最坏），再加结尾 L'\0'；
     *   parseUrl 保证 host <= NETWORK_HOST_BUF_SIZE-1 = 255 字节 -> 最多需 256 个
     *   wchar_t；path <= NETWORK_PATH_BUF_SIZE-1 = 2047 字节 -> 最多需 2048 个 wchar_t。
     *   故宽缓冲按「字节容量 == 宽字符容量」取值即覆盖最坏情形，且随宏放宽自动变大。
     *   容量不足时 utf8ToWide 返回 -3（绝不截断），begin 立即失败并报错。 */
    wchar_t                 whost[NETWORK_HOST_BUF_SIZE];
    wchar_t                 wpath[NETWORK_PATH_BUF_SIZE];
    DWORD                   bodyLen;
    int                     connectTimeoutMs;
    int                     sliceMs;
    int                     err;

    ASSERT_NOT_NULL(instance, "platform_network(WinHTTP) begin: instance is null", -1);
    ASSERT_NOT_NULL(request, "platform_network(WinHTTP) begin: request is null", -2);
    ASSERT_NOT_NULL(request->url, "platform_network(WinHTTP) begin: request->url is null", -3);

    /* 隐式结束上一路未完成会话（幂等） */
    closeSession(self);

    err = parseUrl(request->url, &parsed);
    if (err != 0) {
        /* 具体原因（合法取值范围 / 容量上限 / 实际值 / 正确写法示例）已由
         * parseUrl 自己单条打印，这里只做「本次 begin 因此中止」的定位信息。 */
        log_error("platform_network(WinHTTP) begin: parseUrl failed url=%s err=%d "
                  "(reason: see the preceding parseUrl line; fix the url, retrying the same url will fail again)",
                  request->url, err);
        return -4;
    }
    if (utf8ToWide(parsed.host, whost, (int)(sizeof(whost) / sizeof(wchar_t))) != 0) {
        log_error("platform_network(WinHTTP) begin: host utf8->utf16 failed (host needs more than "
                  "wchar_t[%d])" NETWORK_WIN_BEGIN_CTX_FMT,
                  (int)(sizeof(whost) / sizeof(wchar_t)), NETWORK_WIN_BEGIN_CTX_ARG(parsed, request->url));
        return -5;
    }
    if (utf8ToWide(parsed.path, wpath, (int)(sizeof(wpath) / sizeof(wchar_t))) != 0) {
        log_error("platform_network(WinHTTP) begin: path utf8->utf16 failed (path needs more than "
                  "wchar_t[%d] = NETWORK_PATH_BUF_SIZE)" NETWORK_WIN_BEGIN_CTX_FMT,
                  (int)(sizeof(wpath) / sizeof(wchar_t)), NETWORK_WIN_BEGIN_CTX_ARG(parsed, request->url));
        return -6;
    }

    connectTimeoutMs = (request->connectTimeoutMs > 0) ? request->connectTimeoutMs
                                                       : NETWORK_DEFAULT_CONNECT_TIMEOUT_MS;
    sliceMs          = (request->pollSliceMs > 0) ? request->pollSliceMs
                                                  : NETWORK_DEFAULT_POLL_SLICE_MS;

    /* 1) 打开会话 */
    self->hSession = WinHttpOpen(L"translator-platform-network/1.0",
                                 WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                 WINHTTP_NO_PROXY_NAME,
                                 WINHTTP_NO_PROXY_BYPASS,
                                 0);
    if (self->hSession == NULL) {
        self->lastError = GetLastError();
        log_error("platform_network(WinHTTP) begin: WinHttpOpen failed err=%lu" NETWORK_WIN_BEGIN_CTX_FMT,
                  (unsigned long)self->lastError, NETWORK_WIN_BEGIN_CTX_ARG(parsed, request->url));
        return -7;
    }

    /* 2) 超时：接收超时同时约束「等响应头」，故此处先用较长的 connectTimeoutMs；
     *    等 WinHttpReceiveResponse 返回后，再把接收超时改小为 pollSliceMs。 */
    if (!WinHttpSetTimeouts(self->hSession, connectTimeoutMs, connectTimeoutMs,
                            connectTimeoutMs, connectTimeoutMs)) {
        self->lastError = GetLastError();
        log_error("platform_network(WinHTTP) begin: WinHttpSetTimeouts failed err=%lu" NETWORK_WIN_BEGIN_CTX_FMT,
                  (unsigned long)self->lastError, NETWORK_WIN_BEGIN_CTX_ARG(parsed, request->url));
        closeSession(self);
        return -8;
    }

    /* 3) 建立到主机的连接 */
    self->hConnect = WinHttpConnect(self->hSession, whost, (INTERNET_PORT)parsed.port, 0);
    if (self->hConnect == NULL) {
        self->lastError = GetLastError();
        log_error("platform_network(WinHTTP) begin: WinHttpConnect failed err=%lu" NETWORK_WIN_BEGIN_CTX_FMT,
                  (unsigned long)self->lastError, NETWORK_WIN_BEGIN_CTX_ARG(parsed, request->url));
        closeSession(self);
        return -9;
    }

    /* 4) 打开请求：https 需要 WINHTTP_FLAG_SECURE；无体用 GET，有体用 POST */
    self->hRequest = WinHttpOpenRequest(self->hConnect,
                                        (request->body != NULL && request->bodyLen > 0) ? L"POST" : L"GET",
                                        wpath,
                                        NULL,
                                        WINHTTP_NO_REFERER,
                                        WINHTTP_DEFAULT_ACCEPT_TYPES,
                                        parsed.isHttps ? WINHTTP_FLAG_SECURE : 0);
    if (self->hRequest == NULL) {
        self->lastError = GetLastError();
        log_error("platform_network(WinHTTP) begin: WinHttpOpenRequest failed err=%lu" NETWORK_WIN_BEGIN_CTX_FMT,
                  (unsigned long)self->lastError, NETWORK_WIN_BEGIN_CTX_ARG(parsed, request->url));
        closeSession(self);
        return -10;
    }

    /* 5) Content-Type（可选） */
    if (request->contentType != NULL) {
        char contentTypeHeader[768];
        int  headerLen;

        headerLen = snprintf(contentTypeHeader, sizeof(contentTypeHeader),
                             "Content-Type: %s", request->contentType);
        /* snprintf 返回「本应写入的长度」：>= 缓冲容量即说明被截断，必须报错，
         * 不能把半截 Content-Type 静默发出去（那会让服务端按错误类型解析请求体）。 */
        if (headerLen <= 0 || headerLen >= (int)sizeof(contentTypeHeader)) {
            log_error("platform_network(WinHTTP) begin: Content-Type too long: need=%d bytes "
                      "limit=%d bytes" NETWORK_WIN_BEGIN_CTX_FMT,
                      headerLen, (int)sizeof(contentTypeHeader),
                      NETWORK_WIN_BEGIN_CTX_ARG(parsed, request->url));
            closeSession(self);
            return -11;
        }
        if (addHeaderUtf8(self->hRequest, contentTypeHeader) != 0) {
            log_error("platform_network(WinHTTP) begin: add Content-Type failed err=%lu" NETWORK_WIN_BEGIN_CTX_FMT,
                      (unsigned long)GetLastError(), NETWORK_WIN_BEGIN_CTX_ARG(parsed, request->url));
            closeSession(self);
            return -12;
        }
    }

    /* 6) Basic 鉴权（可选）：直接带凭据，语义等同 curl -u user:password；
     *    不使用 WinHttpSetCredentials（它要等 401 挑战，语义不同）。 */
    if (request->authUser != NULL) {
        char authHeader[768];

        if (buildBasicAuthHeader(request->authUser, request->authPassword,
                                 authHeader, (int)sizeof(authHeader)) != 0) {
            log_error("platform_network(WinHTTP) begin: build basic auth failed "
                      "(user/password too long for %d-byte header buffer)" NETWORK_WIN_BEGIN_CTX_FMT,
                      (int)sizeof(authHeader), NETWORK_WIN_BEGIN_CTX_ARG(parsed, request->url));
            closeSession(self);
            return -13;
        }
        if (addHeaderUtf8(self->hRequest, authHeader) != 0) {
            log_error("platform_network(WinHTTP) begin: add Authorization failed err=%lu" NETWORK_WIN_BEGIN_CTX_FMT,
                      (unsigned long)GetLastError(), NETWORK_WIN_BEGIN_CTX_ARG(parsed, request->url));
            closeSession(self);
            return -14;
        }
    }

    /* 7) 发送请求（带请求体） */
    bodyLen = (request->body != NULL && request->bodyLen > 0) ? (DWORD)request->bodyLen : 0;
    if (!WinHttpSendRequest(self->hRequest,
                            WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            (LPVOID)request->body, bodyLen, bodyLen, 0)) {
        self->lastError = GetLastError();
        log_error("platform_network(WinHTTP) begin: WinHttpSendRequest failed err=%lu" NETWORK_WIN_BEGIN_CTX_FMT,
                  (unsigned long)self->lastError, NETWORK_WIN_BEGIN_CTX_ARG(parsed, request->url));
        closeSession(self);
        return -15;
    }

    /* 8) 等响应头（受接收超时约束，此处仍是较长的 connectTimeoutMs） */
    if (!WinHttpReceiveResponse(self->hRequest, NULL)) {
        self->lastError = GetLastError();
        log_error("platform_network(WinHTTP) begin: WinHttpReceiveResponse failed err=%lu" NETWORK_WIN_BEGIN_CTX_FMT,
                  (unsigned long)self->lastError, NETWORK_WIN_BEGIN_CTX_ARG(parsed, request->url));
        closeSession(self);
        return -16;
    }

    /* 9) 响应头已到：把接收超时改小，使后续 poll 能快速返回，
     *    不会一次卡住整路请求。此处失败不影响已建立的会话，仅记录。 */
    if (!WinHttpSetTimeouts(self->hSession, sliceMs, sliceMs, sliceMs, sliceMs)) {
        log_error("platform_network(WinHTTP) begin: reset receive timeout failed err=%lu" NETWORK_WIN_BEGIN_CTX_FMT,
                  (unsigned long)GetLastError(), NETWORK_WIN_BEGIN_CTX_ARG(parsed, request->url));
    }

    return 0;
}

/* 泵送一次网络 I/O 并取增量响应体（判据与 MSDN 官方样例一致） */
static int poll(void* instance, void* out, int outCap, int* isDone)
{
    PlatformNetworkWindows* self = (PlatformNetworkWindows*)instance;
    DWORD                   available = 0;
    DWORD                   toRead;
    DWORD                   readBytes = 0;

    ASSERT_NOT_NULL(instance, "platform_network(WinHTTP) poll: instance is null", -1);
    ASSERT_NOT_NULL(isDone, "platform_network(WinHTTP) poll: isDone is null", -2);

    *isDone = 0;

    if (out != NULL && outCap <= 0) {
        log_error("platform_network(WinHTTP) poll: outCap<=0 but out is not null");
        return -3;
    }
    if (self->hRequest == NULL) {
        log_error("platform_network(WinHTTP) poll: no active session");
        return -4;
    }

    if (!WinHttpQueryDataAvailable(self->hRequest, &available)) {
        DWORD err = GetLastError();

        /* 接收超时：本次切片内暂无数据，仍在进行 -> 返回 0，交由调用方稍后再泵 */
        if (err == ERROR_WINHTTP_TIMEOUT) {
            return 0;
        }
        self->lastError = err;
        log_error("platform_network(WinHTTP) poll: WinHttpQueryDataAvailable failed err=%lu",
                  (unsigned long)err);
        return -5;
    }

    /* 可用字节数为 0：响应已结束（含对端正常关闭） */
    if (available == 0) {
        *isDone = 1;
        return 0;
    }

    /* out == NULL：只泵送不取数据。不调用 WinHttpReadData，数据留在 WinHTTP 内部缓冲，
     * 下一次 poll 再取，绝不丢。 */
    if (out == NULL) {
        return 0;
    }

    toRead = available;
    if ((DWORD)outCap < toRead) {
        toRead = (DWORD)outCap;
    }

    if (!WinHttpReadData(self->hRequest, out, toRead, &readBytes)) {
        self->lastError = GetLastError();
        log_error("platform_network(WinHTTP) poll: WinHttpReadData failed err=%lu",
                  (unsigned long)self->lastError);
        return -6;
    }

    return (int)readBytes;
}

/* HTTP 状态码查询（宽字符判据；len 单位为字节） */
static int statusCode(void* instance)
{
    PlatformNetworkWindows* self = (PlatformNetworkWindows*)instance;
    DWORD                   code = 0;
    DWORD                   len  = (DWORD)sizeof(code);

    ASSERT_NOT_NULL(instance, "platform_network(WinHTTP) statusCode: instance is null", -1);

    if (self->hRequest == NULL) {
        return -1;
    }

    if (!WinHttpQueryHeaders(self->hRequest,
                             WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX,
                             &code, &len, WINHTTP_NO_HEADER_INDEX)) {
        return 0;   /* 尚未收到响应头 */
    }

    return (int)code;
}

/* 释放会话与实例（幂等） */
static void deleteInstance(void* instance)
{
    PlatformNetworkWindows* self;

    if (instance == NULL) {
        return;
    }
    self = (PlatformNetworkWindows*)instance;

    closeSession(self);

    /* 销毁具体实例（抽象包装器由 DeletePlatformNetwork 释放） */
    networkFree(self);
}

/* 组装 Windows 具象虚表 */
static PlatformNetworkInterface windowsInterface(void)
{
    PlatformNetworkInterface interfaces;

    interfaces.begin          = begin;
    interfaces.poll           = poll;
    interfaces.statusCode     = statusCode;
    interfaces.deleteInstance = deleteInstance;

    return interfaces;
}

/* Windows 具体构造：装配具体实例 + 调抽象构造完成包装（禁止手工赋值） */
static PlatformNetwork* NewPlatformNetworkWindows(void)
{
    PlatformNetworkWindows* self;
    PlatformNetwork*        network;

    self = (PlatformNetworkWindows*)networkMalloc(sizeof(PlatformNetworkWindows));
    ASSERT_NOT_NULL(self, "NewPlatformNetworkWindows: networkMalloc failed", NULL);
    memset(self, 0, sizeof(PlatformNetworkWindows));

    network = NewPlatformNetwork(self, windowsInterface());
    if (network == NULL) {
        networkFree(self);
        self = NULL;
        return network;
    }

    return network;
}

/* 平台中立工厂：Windows -> WinHTTP 具象 */
PlatformNetwork* NewPlatformNetworkNative(void)
{
    return NewPlatformNetworkWindows();
}

/* =====================================================================
 * 平台具象：Linux / macOS（BSD socket，仅明文 http）
 * ===================================================================== */
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdarg.h>

#define NETWORK_POSIX_REQ_BUF_SIZE   4096
#define NETWORK_POSIX_HEADER_BUF_SIZE 4096
#define NETWORK_POSIX_BODY_BUF_SIZE   4096

/* 具体实现实例（刻意定义在此文件作用域内，绝不进头文件，理由见文件顶部说明） */
typedef struct PlatformNetworkPosix{
    int  fd;                                    /* 会话套接字；-1 = 无会话 */
    int  statusCode;                            /* 0 = 尚未收到响应头 */
    int  headerDone;                            /* 响应头是否已收完并解析 */
    int  headerLen;                             /* headerBuf 中已累积字节数 */
    int  bodyLen;                               /* bodyBuf 中残留（随头一起读入）的 body 字节 */
    int  pollSliceMs;                           /* 单次 poll 的有界等待上限 */
    char headerBuf[NETWORK_POSIX_HEADER_BUF_SIZE];
    char bodyBuf[NETWORK_POSIX_BODY_BUF_SIZE];
} PlatformNetworkPosix;

static int  begin(void* instance, const PlatformNetworkRequest* request);
static int  poll(void* instance, void* out, int outCap, int* isDone);
static int  statusCode(void* instance);
static void deleteInstance(void* instance);
static void closeSession(PlatformNetworkPosix* self);
static int  setNonBlocking(int fd);
static int  connectWithTimeout(int fd, const struct sockaddr* addr, socklen_t addrLen,
                               int timeoutMs, int sliceMs);
static int  waitReadable(int fd, int timeoutMs);
static int  sendAll(int fd, const char* buf, int len, int timeoutMs);
static int  appendFmt(char* buf, int cap, int* len, const char* fmt, ...);
static void parseStatusCode(PlatformNetworkPosix* self);

/* begin 失败日志的统一上下文后缀（parseUrl 成功之后的任何失败都带上它）：
 * 一条日志即含 host/port/path/url，调用方无需回看上一行就能定位「哪个地址的哪一步」失败。
 * 用法：log_error("... errno=%d" NETWORK_POSIX_BEGIN_CTX_FMT,
 *                errno, NETWORK_POSIX_BEGIN_CTX_ARG(parsed, request->url));  */
#define NETWORK_POSIX_BEGIN_CTX_FMT  " host=%s port=%d path=%s url=%s"
#define NETWORK_POSIX_BEGIN_CTX_ARG(p, reqUrl) \
    ((p).host), ((p).port), ((p).path), (reqUrl)

/* 幂等关闭套接字并复位会话状态 */
static void closeSession(PlatformNetworkPosix* self)
{
    if (self == NULL) {
        return;
    }
    if (self->fd >= 0) {
        close(self->fd);      /* POSIX 用 close；Windows 分支用 WinHttpCloseHandle，互不相关 */
        self->fd = -1;
    }
    self->statusCode = 0;
    self->headerDone = 0;
    self->headerLen  = 0;
    self->bodyLen    = 0;
}

/* 设为非阻塞 */
static int setNonBlocking(int fd)
{
    int flags;

    flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return -2;
    }

    return 0;
}

/* 非阻塞连接：以 sliceMs 为单次 select 切片，累计到 timeoutMs 仍未就绪则判超时；
 * select 报告可写后用 SO_ERROR 判定连接结果。返回 0 成功，<0 失败。 */
static int connectWithTimeout(int fd, const struct sockaddr* addr, socklen_t addrLen,
                              int timeoutMs, int sliceMs)
{
    int elapsed = 0;
    int slice   = (sliceMs > 0) ? sliceMs : NETWORK_DEFAULT_POLL_SLICE_MS;

    if (connect(fd, addr, addrLen) == 0) {
        return 0;
    }
    if (errno != EINPROGRESS) {
        return -1;
    }

    while (elapsed < timeoutMs) {
        fd_set         wfds;
        struct timeval tv;
        int            sel;
        int            soErr = 0;
        socklen_t      soErrLen = (socklen_t)sizeof(soErr);

        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);
        tv.tv_sec  = slice / 1000;
        tv.tv_usec = (slice % 1000) * 1000;

        sel = select(fd + 1, NULL, &wfds, NULL, &tv);
        if (sel < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -2;
        }
        if (sel == 0) {
            elapsed += slice;      /* 本切片无进展，累计后继续等，直到总超时 */
            continue;
        }
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soErr, &soErrLen) != 0) {
            return -3;
        }
        if (soErr != 0) {
            return -4;
        }
        return 0;                  /* 连接成功 */
    }

    return -5;                     /* 连接超时 */
}

/* 有界等待可读；>0 可读，0 超时，<0 出错 */
static int waitReadable(int fd, int timeoutMs)
{
    fd_set         rfds;
    struct timeval tv;

    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    tv.tv_sec  = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;

    return select(fd + 1, &rfds, NULL, NULL, &tv);
}

/* 非阻塞 + select 的可写等待，把全部字节发出。返回 0 成功，<0 失败。 */
static int sendAll(int fd, const char* buf, int len, int timeoutMs)
{
    int sent = 0;

    if (fd < 0 || buf == NULL || len < 0) {
        return -1;
    }

    while (sent < len) {
        fd_set         wfds;
        struct timeval tv;
        int            sel;
        int            n;

        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);
        tv.tv_sec  = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;

        sel = select(fd + 1, NULL, &wfds, NULL, &tv);
        if (sel < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -2;
        }
        if (sel == 0) {
            return -3;             /* 发送超时 */
        }

        n = (int)send(fd, buf + sent, (size_t)(len - sent), 0);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            return -4;
        }
        if (n == 0) {
            return -5;
        }
        sent += n;
    }

    return 0;
}

/* 逐段拼装请求文本；返回 0 成功，<0 失败（缓冲不足） */
static int appendFmt(char* buf, int cap, int* len, const char* fmt, ...)
{
    va_list ap;
    int     n;

    if (buf == NULL || len == NULL || fmt == NULL || cap <= *len) {
        return -1;
    }

    va_start(ap, fmt);
    n = vsnprintf(buf + *len, (size_t)(cap - *len), fmt, ap);
    va_end(ap);

    if (n < 0 || n >= cap - *len) {
        return -2;
    }
    *len += n;

    return 0;
}

/* 解析响应首行 "HTTP/1.x <code> ..." 的状态码；解析不出则保持 0 */
static void parseStatusCode(PlatformNetworkPosix* self)
{
    const char* cursor = self->headerBuf;
    int         code   = 0;

    while (*cursor != '\0' && *cursor != ' ') {
        cursor++;
    }
    while (*cursor == ' ') {
        cursor++;
    }

    if (cursor[0] >= '0' && cursor[0] <= '9' &&
        cursor[1] >= '0' && cursor[1] <= '9' &&
        cursor[2] >= '0' && cursor[2] <= '9') {
        code = (cursor[0] - '0') * 100 + (cursor[1] - '0') * 10 + (cursor[2] - '0');
    }

    self->statusCode = code;
}

/* 发起请求：解析 -> 建连 -> 发请求；发起后即可由 poll 增量收取响应 */
static int begin(void* instance, const PlatformNetworkRequest* request)
{
    PlatformNetworkPosix* self = (PlatformNetworkPosix*)instance;
    NetworkParsedUrl      parsed;
    struct addrinfo       hints;
    struct addrinfo*      result = NULL;
    struct addrinfo*      rp     = NULL;
    int                   rc;
    int                   fd = -1;
    int                   connectTimeoutMs;
    int                   sliceMs;
    int                   bodyLen;
    int                   hostRc;
    const char*           method;
    /* req 容量核算（最坏情形，均已含结尾 '\0'）：
     *   请求行 "POST "4+1 + path(<=2047) + " HTTP/1.0\r\n"10  = 2063
     *   Host     "Host: "6 + hostPort(<=261) + "\r\n"2          =  269
     *   Content-Length "Content-Length: "16 + 11 + "\r\n"2      =   29
     *   Authorization  "Authorization: Basic "21 + 684 + 2      =  707
     *   尾部 "Connection: close\r\n\r\n"                          =   21
     *   合计 3089 字节，余下 1007 字节供 Content-Type；若 Content-Type 更长，
     *   appendFmt 会返回 <0 并使 begin 失败（绝不静默截断）。
     *   NETWORK_POSIX_REQ_BUF_SIZE(4096) 足够覆盖以上最坏情形。 */
    char                  req[NETWORK_POSIX_REQ_BUF_SIZE];
    int                   reqLen = 0;
    /* hostPort 容量 = NETWORK_HOST_BUF_SIZE + 16 = 272 字节，覆盖
     * 「255 字节 host + ':' + 5 位端口(65535) + '\0' = 262」的最坏情形。 */
    char                  hostPort[NETWORK_HOST_BUF_SIZE + 16];
    /* Authorization 头最长 = "Authorization: Basic "21 + base64((512-1)/3*4=684) + '\0' = 706 */
    char                  authHeader[768];

    ASSERT_NOT_NULL(instance, "platform_network(posix) begin: instance is null", -1);
    ASSERT_NOT_NULL(request, "platform_network(posix) begin: request is null", -2);
    ASSERT_NOT_NULL(request->url, "platform_network(posix) begin: request->url is null", -3);

    /* 隐式结束上一路未完成会话（幂等） */
    closeSession(self);

    rc = parseUrl(request->url, &parsed);
    if (rc != 0) {
        /* 具体原因（合法取值范围 / 容量上限 / 实际值 / 正确写法示例）已由
         * parseUrl 自己单条打印，这里只做「本次 begin 因此中止」的定位信息。 */
        log_error("platform_network(posix) begin: parseUrl failed url=%s err=%d "
                  "(reason: see the preceding parseUrl line; fix the url, retrying the same url will fail again)",
                  request->url, rc);
        return -4;
    }

    /* 本平台仅支持明文 http：不引入 OpenSSL 依赖 */
    if (parsed.isHttps) {
        log_error("platform_network(posix): https is not supported on linux/macos (no TLS backend); "
                  "use \"http://\" or terminate TLS at a local proxy" NETWORK_POSIX_BEGIN_CTX_FMT,
                  NETWORK_POSIX_BEGIN_CTX_ARG(parsed, request->url));
        return -5;
    }

    connectTimeoutMs = (request->connectTimeoutMs > 0) ? request->connectTimeoutMs
                                                       : NETWORK_DEFAULT_CONNECT_TIMEOUT_MS;
    sliceMs          = (request->pollSliceMs > 0) ? request->pollSliceMs
                                                  : NETWORK_DEFAULT_POLL_SLICE_MS;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    {
        char portStr[16];   /* 容量核算：parseUrl 已保证端口 1..65535（最多 5 位）+ '\0'，16 足够 */

        snprintf(portStr, sizeof(portStr), "%d", parsed.port);
        rc = getaddrinfo(parsed.host, portStr, &hints, &result);
        if (rc != 0 || result == NULL) {
            log_error("platform_network(posix) begin: getaddrinfo failed rc=%d" NETWORK_POSIX_BEGIN_CTX_FMT,
                      rc, NETWORK_POSIX_BEGIN_CTX_ARG(parsed, request->url));
            if (result != NULL) {
                freeaddrinfo(result);
                result = NULL;
            }
            return -6;
        }
    }

    for (rp = result; rp != NULL; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (setNonBlocking(fd) != 0) {
            close(fd);
            fd = -1;
            continue;
        }
        if (connectWithTimeout(fd, rp->ai_addr, (socklen_t)rp->ai_addrlen,
                               connectTimeoutMs, sliceMs) == 0) {
            break;      /* 连接成功 */
        }
        close(fd);
        fd = -1;
    }

    freeaddrinfo(result);
    result = NULL;

    if (fd < 0) {
        log_error("platform_network(posix) begin: connect failed (all addresses tried, timeoutMs=%d)"
                  NETWORK_POSIX_BEGIN_CTX_FMT,
                  connectTimeoutMs, NETWORK_POSIX_BEGIN_CTX_ARG(parsed, request->url));
        return -7;
    }

    /* 组装请求：HTTP/1.0 + Connection: close，响应体以「连接关闭」为界，
     * 无需实现 chunked 传输编码解码。 */
    method  = (request->body != NULL && request->bodyLen > 0) ? "POST" : "GET";
    bodyLen = (request->body != NULL && request->bodyLen > 0) ? request->bodyLen : 0;

    if (parsed.port != 80) {
        hostRc = snprintf(hostPort, sizeof(hostPort), "%s:%d", parsed.host, parsed.port);
    } else {
        hostRc = snprintf(hostPort, sizeof(hostPort), "%s", parsed.host);
    }
    /* snprintf 返回「本应写入的长度」：>= 缓冲容量即说明被截断。Host 头被截断会
     * 让服务端解析到错误主机名，故必须报错而不是继续。容量核算见上方声明处的注释。 */
    if (hostRc <= 0 || (size_t)hostRc >= sizeof(hostPort)) {
        log_error("platform_network(posix) begin: build Host header failed: need=%d bytes limit=%d bytes"
                  NETWORK_POSIX_BEGIN_CTX_FMT,
                  hostRc, (int)sizeof(hostPort), NETWORK_POSIX_BEGIN_CTX_ARG(parsed, request->url));
        close(fd);
        return -8;      /* 复用「组装请求失败」错误码，不新增对外可见的返回值 */
    }

    if (appendFmt(req, (int)sizeof(req), &reqLen, "%s %s HTTP/1.0\r\n", method, parsed.path) != 0 ||
        appendFmt(req, (int)sizeof(req), &reqLen, "Host: %s\r\n", hostPort) != 0) {
        log_error("platform_network(posix) begin: build request line failed "
                  "(request buffer=%d bytes exceeded)" NETWORK_POSIX_BEGIN_CTX_FMT,
                  (int)sizeof(req), NETWORK_POSIX_BEGIN_CTX_ARG(parsed, request->url));
        close(fd);
        return -8;
    }

    if (request->contentType != NULL &&
        appendFmt(req, (int)sizeof(req), &reqLen, "Content-Type: %s\r\n", request->contentType) != 0) {
        log_error("platform_network(posix) begin: build Content-Type failed (request buffer=%d bytes exceeded, "
                  "contentType is too long; remaining=%d)" NETWORK_POSIX_BEGIN_CTX_FMT,
                  (int)sizeof(req), (int)sizeof(req) - reqLen,
                  NETWORK_POSIX_BEGIN_CTX_ARG(parsed, request->url));
        close(fd);
        return -9;
    }

    if (bodyLen > 0 &&
        appendFmt(req, (int)sizeof(req), &reqLen, "Content-Length: %d\r\n", bodyLen) != 0) {
        log_error("platform_network(posix) begin: build Content-Length failed" NETWORK_POSIX_BEGIN_CTX_FMT,
                  NETWORK_POSIX_BEGIN_CTX_ARG(parsed, request->url));
        close(fd);
        return -10;
    }

    if (request->authUser != NULL) {
        if (buildBasicAuthHeader(request->authUser, request->authPassword,
                                 authHeader, (int)sizeof(authHeader)) != 0 ||
            appendFmt(req, (int)sizeof(req), &reqLen, "%s\r\n", authHeader) != 0) {
            log_error("platform_network(posix) begin: build Authorization failed "
                      "(user/password too long for %d-byte buffer, or request buffer=%d bytes exceeded)"
                      NETWORK_POSIX_BEGIN_CTX_FMT,
                      (int)sizeof(authHeader), (int)sizeof(req),
                      NETWORK_POSIX_BEGIN_CTX_ARG(parsed, request->url));
            close(fd);
            return -11;
        }
    }

    if (appendFmt(req, (int)sizeof(req), &reqLen, "Connection: close\r\n\r\n") != 0) {
        log_error("platform_network(posix) begin: build request tail failed" NETWORK_POSIX_BEGIN_CTX_FMT,
                  NETWORK_POSIX_BEGIN_CTX_ARG(parsed, request->url));
        close(fd);
        return -12;
    }

    if (sendAll(fd, req, reqLen, connectTimeoutMs) != 0) {
        log_error("platform_network(posix) begin: send request header failed (sent=%d bytes, timeoutMs=%d, errno=%d)"
                  NETWORK_POSIX_BEGIN_CTX_FMT,
                  reqLen, connectTimeoutMs, errno, NETWORK_POSIX_BEGIN_CTX_ARG(parsed, request->url));
        close(fd);
        return -13;
    }
    if (bodyLen > 0 && sendAll(fd, (const char*)request->body, bodyLen, connectTimeoutMs) != 0) {
        log_error("platform_network(posix) begin: send request body failed (bodyLen=%d, timeoutMs=%d, errno=%d)"
                  NETWORK_POSIX_BEGIN_CTX_FMT,
                  bodyLen, connectTimeoutMs, errno, NETWORK_POSIX_BEGIN_CTX_ARG(parsed, request->url));
        close(fd);
        return -14;
    }

    self->fd           = fd;
    self->statusCode   = 0;
    self->headerDone   = 0;
    self->headerLen    = 0;
    self->bodyLen      = 0;
    self->pollSliceMs  = sliceMs;

    return 0;
}

/* 泵送一次网络 I/O 并取增量响应体：先收完响应头并解析状态码，再增量返回 body */
static int poll(void* instance, void* out, int outCap, int* isDone)
{
    PlatformNetworkPosix* self = (PlatformNetworkPosix*)instance;
    int                   slice;

    ASSERT_NOT_NULL(instance, "platform_network(posix) poll: instance is null", -1);
    ASSERT_NOT_NULL(isDone, "platform_network(posix) poll: isDone is null", -2);

    *isDone = 0;

    if (out != NULL && outCap <= 0) {
        log_error("platform_network(posix) poll: outCap<=0 but out is not null");
        return -3;
    }
    if (self->fd < 0) {
        log_error("platform_network(posix) poll: no active session");
        return -4;
    }

    slice = (self->pollSliceMs > 0) ? self->pollSliceMs : NETWORK_DEFAULT_POLL_SLICE_MS;

    /* 阶段一：增量收完响应头，直到出现 "\r\n\r\n"，并解析状态码 */
    if (!self->headerDone) {
        for (;;) {
            int   ready;
            int   n;
            char* headEnd;

            if (self->headerLen >= (int)sizeof(self->headerBuf) - 1) {
                log_error("platform_network(posix) poll: response header too large");
                return -5;
            }

            ready = waitReadable(self->fd, slice);
            if (ready == 0) {
                return 0;                       /* 本次切片暂无数据 */
            }
            if (ready < 0) {
                log_error("platform_network(posix) poll: select(header) failed errno=%d", errno);
                return -6;
            }

            n = (int)recv(self->fd,
                          self->headerBuf + self->headerLen,
                          (size_t)((int)sizeof(self->headerBuf) - 1 - self->headerLen),
                          0);
            if (n == 0) {
                log_error("platform_network(posix) poll: connection closed while reading header");
                return -7;
            }
            if (n < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                    continue;
                }
                log_error("platform_network(posix) poll: recv(header) failed errno=%d", errno);
                return -8;
            }

            self->headerLen += n;
            self->headerBuf[self->headerLen] = '\0';

            headEnd = strstr(self->headerBuf, "\r\n\r\n");
            if (headEnd == NULL) {
                continue;                       /* 头部未收完，继续 */
            }

            /* 头部结束：解析状态码，并把随头部一起读入的 body 残留搬到 bodyBuf，
             * 绝不丢（out == NULL 时同样不得丢）。 */
            {
                int headerBytes = (int)(headEnd + 4 - self->headerBuf);
                int leftover    = self->headerLen - headerBytes;

                parseStatusCode(self);
                self->headerDone = 1;
                self->bodyLen    = 0;
                if (leftover > 0) {
                    if (leftover > (int)sizeof(self->bodyBuf)) {
                        leftover = (int)sizeof(self->bodyBuf);
                    }
                    memcpy(self->bodyBuf, self->headerBuf + headerBytes, (size_t)leftover);
                    self->bodyLen = leftover;
                }
            }
            break;
        }
    }

    /* 阶段二：优先吐出「随头部一起读到」的 body 残留 */
    if (self->bodyLen > 0) {
        if (out != NULL) {
            int take = self->bodyLen;

            if (take > outCap) {
                take = outCap;
            }
            memcpy(out, self->bodyBuf, (size_t)take);
            if (take < self->bodyLen) {
                memmove(self->bodyBuf, self->bodyBuf + take, (size_t)(self->bodyLen - take));
            }
            self->bodyLen -= take;
            return take;
        }
        return 0;                               /* out == NULL：数据留在 bodyBuf，不丢 */
    }

    /* 阶段三：从套接字增量收 body */
    {
        int ready = waitReadable(self->fd, slice);

        if (ready == 0) {
            return 0;
        }
        if (ready < 0) {
            log_error("platform_network(posix) poll: select(body) failed errno=%d", errno);
            return -9;
        }

        if (out == NULL) {
            /* 只泵送不取数据：用 MSG_PEEK 探测对端是否关闭，不消费任何字节 */
            char peekByte;
            int  peeked = (int)recv(self->fd, &peekByte, 1, MSG_PEEK);

            if (peeked == 0) {
                *isDone = 1;
            }
            return 0;
        }

        for (;;) {
            int n = (int)recv(self->fd, out, (size_t)outCap, 0);

            if (n > 0) {
                return n;
            }
            if (n == 0) {
                *isDone = 1;                    /* 对端关闭 = body 结束 */
                return 0;
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return 0;
            }
            log_error("platform_network(posix) poll: recv(body) failed errno=%d", errno);
            return -10;
        }
    }
}

/* HTTP 状态码；尚未收到响应头返回 0，无会话返回 <0 */
static int statusCode(void* instance)
{
    PlatformNetworkPosix* self = (PlatformNetworkPosix*)instance;

    ASSERT_NOT_NULL(instance, "platform_network(posix) statusCode: instance is null", -1);

    if (self->fd < 0) {
        return -1;
    }

    return self->statusCode;
}

/* 释放会话与实例（幂等） */
static void deleteInstance(void* instance)
{
    PlatformNetworkPosix* self;

    if (instance == NULL) {
        return;
    }
    self = (PlatformNetworkPosix*)instance;

    closeSession(self);

    /* 销毁具体实例（抽象包装器由 DeletePlatformNetwork 释放） */
    networkFree(self);
}

/* 组装 POSIX 具象虚表 */
static PlatformNetworkInterface posixInterface(void)
{
    PlatformNetworkInterface interfaces;

    interfaces.begin          = begin;
    interfaces.poll           = poll;
    interfaces.statusCode     = statusCode;
    interfaces.deleteInstance = deleteInstance;

    return interfaces;
}

/* POSIX 具体构造：装配具体实例 + 调抽象构造完成包装（禁止手工赋值） */
static PlatformNetwork* NewPlatformNetworkPosix(void)
{
    PlatformNetworkPosix* self;
    PlatformNetwork*      network;

    self = (PlatformNetworkPosix*)networkMalloc(sizeof(PlatformNetworkPosix));
    ASSERT_NOT_NULL(self, "NewPlatformNetworkPosix: networkMalloc failed", NULL);
    memset(self, 0, sizeof(PlatformNetworkPosix));
    self->fd = -1;      /* 无会话哨兵 */

    network = NewPlatformNetwork(self, posixInterface());
    if (network == NULL) {
        networkFree(self);
        self = NULL;
        return network;
    }

    return network;
}

/* 平台中立工厂：Linux / macOS -> BSD socket 具象 */
PlatformNetwork* NewPlatformNetworkNative(void)
{
    return NewPlatformNetworkPosix();
}

/* =====================================================================
 * 其它平台（FreeRTOS 等）：无网络能力
 * ===================================================================== */
#else

/* 保证任何平台都能链接通过：工厂返回 NULL。抽象构造 / DeletePlatformNetwork
 * 仍然可用（上层可自行注入自备的具体实例）。 */
PlatformNetwork* NewPlatformNetworkNative(void)
{
    log_error("NewPlatformNetworkNative: network not available on this platform");
    return NULL;
}

#endif /* 平台互斥分支 */

#endif /* PLATFORM_USE_NETWORK */
