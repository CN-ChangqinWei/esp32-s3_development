#ifndef _PLATFORM_H
#define _PLATFORM_H

/* =====================================================================
 * platform —— 跨平台适配门面（契约头 / 全模块唯一共享头）
 *
 * 职责：
 *   1. 平台选择宏：PLATFORM_WINDOWS / PLATFORM_LINUX / PLATFORM_MACOS / PLATFORM_FREERTOS
 *      - 编译期可用 -DPLATFORM_XXX=1 显式指定；
 *      - 未显式指定时按编译器内置宏自动探测；
 *      - 一个都探测不到则 #error 中止，避免静默走错分支。
 *   2. 能力开关宏：PLATFORM_USE_THREAD_OBJECT / PLATFORM_USE_MEMORY / PLATFORM_USE_TIMESTAMP
 *                  / PLATFORM_USE_NETWORK / PLATFORM_USE_SLEEP / PLATFORM_USE_SYNC
 *                  / PLATFORM_USE_SCREENCAPTURE / PLATFORM_USE_AUDIO_CAPTURE / PLATFORM_USE_WEBSOCKET
 *      - 用于「是否导入对应的模块头文件、是否创建对应的函数」；
 *      - 置 0 时对应头文件不会被包含，对应 NewPlatformXXX 也不会被声明（实现侧同样不生成）。
 *   3. 聚合包含：打开 platform.h 即可拿到全部已启用能力的对外接口。
 *
 * 使用约定：
 *   - 本头文件由编排者统一维护（共享契约），各能力实现（platform_thread.c /
 *     platform_memory.c / platform_timestamp.c / platform_network.c /
 *     platform_sleep.c / platform_sync.c / platform_screencapture.c）只允许引用，
 *     不允许各自重复定义平台宏；
 *   - 平台宏必须在聚合 include 之前定义完（因此聚合 include 放在文件末尾），
 *     这样 platform_xxx.h 反向 #include "platform.h" 时能安全命中 include guard。
 * ===================================================================== */

/* ---------------------------------------------------------------------
 * 1. 平台选择宏（可被 -D 覆盖；未指定则自动探测）
 * ------------------------------------------------------------------- */
#if !defined(PLATFORM_WINDOWS) && !defined(PLATFORM_LINUX) && \
    !defined(PLATFORM_MACOS)   && !defined(PLATFORM_FREERTOS)

#  if defined(_WIN32) || defined(_WIN64)
#    define PLATFORM_WINDOWS   1
#  elif defined(__APPLE__) && defined(__MACH__)
#    define PLATFORM_MACOS     1
#  elif defined(ESP_PLATFORM) || defined(USE_FREERTOS) || defined(FREERTOS_TARGET)
#    define PLATFORM_FREERTOS  1
#  elif defined(__linux__)
#    define PLATFORM_LINUX     1
#  endif

#endif /* 自动探测 */

#if !defined(PLATFORM_WINDOWS) && !defined(PLATFORM_LINUX) && \
    !defined(PLATFORM_MACOS)   && !defined(PLATFORM_FREERTOS)
#  error "platform: 未能识别当前平台；请显式定义 PLATFORM_WINDOWS / PLATFORM_LINUX / PLATFORM_MACOS / PLATFORM_FREERTOS 之一"
#endif

/* 平台枚举值（便于运行期比较 / 日志） */
#define PLATFORM_ID_WINDOWS   1
#define PLATFORM_ID_LINUX     2
#define PLATFORM_ID_MACOS     3
#define PLATFORM_ID_FREERTOS  4

#if defined(PLATFORM_WINDOWS)
#  define PLATFORM_TYPE  PLATFORM_ID_WINDOWS
#  define PLATFORM_NAME  "windows"
#elif defined(PLATFORM_LINUX)
#  define PLATFORM_TYPE  PLATFORM_ID_LINUX
#  define PLATFORM_NAME  "linux"
#elif defined(PLATFORM_MACOS)
#  define PLATFORM_TYPE  PLATFORM_ID_MACOS
#  define PLATFORM_NAME  "macos"
#elif defined(PLATFORM_FREERTOS)
#  define PLATFORM_TYPE  PLATFORM_ID_FREERTOS
#  define PLATFORM_NAME  "freertos"
#endif

/* ---------------------------------------------------------------------
 * 2. 能力开关（默认全开；置 0 即裁剪该能力，不导入其头文件、不生成其函数）
 * ------------------------------------------------------------------- */
#ifndef PLATFORM_USE_THREAD_OBJECT
#define PLATFORM_USE_THREAD_OBJECT 1
#endif

#ifndef PLATFORM_USE_MEMORY
#define PLATFORM_USE_MEMORY 1
#endif

#ifndef PLATFORM_USE_TIMESTAMP
#define PLATFORM_USE_TIMESTAMP 1
#endif

#ifndef PLATFORM_USE_NETWORK
#define PLATFORM_USE_NETWORK 1
#endif

#ifndef PLATFORM_USE_SLEEP
#define PLATFORM_USE_SLEEP 1
#endif

#ifndef PLATFORM_USE_SYNC
#define PLATFORM_USE_SYNC 1
#endif

/* 屏幕捕获能力：Windows 提供 GDI BitBlt 具象；
 * 其余平台（Linux / macOS / FreeRTOS）暂无具象，NewPlatformScreenCaptureNative() 返回 NULL。
 * 聚合包含见文件末尾第 3 节（platform_screencapture.h）。 */
#ifndef PLATFORM_USE_SCREENCAPTURE
#define PLATFORM_USE_SCREENCAPTURE 1
#endif

/* 音频采集能力：Windows 提供 WASAPI 具象（系统环回 / 麦克风，独立采集线程 + 环形缓冲，
 * 输出统一转码为调用方请求的采样率 / 声道 / 位深）；
 * 其余平台（Linux / macOS / FreeRTOS）暂无具象，NewPlatformAudioCaptureNative() 返回 NULL。
 * 聚合包含见文件末尾第 3 节（platform_audio_capture.h）。 */
#ifndef PLATFORM_USE_AUDIO_CAPTURE
#define PLATFORM_USE_AUDIO_CAPTURE 1
#endif

/* WebSocket 客户端能力：Windows 提供具象（WinHTTP WebSocket）；
 * 其余平台（Linux / macOS / FreeRTOS）暂无具象，NewPlatformWebSocketNative() 返回 NULL。
 * 聚合包含见文件末尾第 3 节（platform_websocket.h）。 */
#ifndef PLATFORM_USE_WEBSOCKET
#define PLATFORM_USE_WEBSOCKET 1
#endif

/* ---------------------------------------------------------------------
 * 3. 聚合包含（按能力开关裁剪）
 *
 * C 链接包裹（防御性）：本头会被 C++ 编译单元（Qt 具象层、demo）包含，
 * 故把 extern "C" 开在聚合包含之前、收在文件末尾，让聚合引入的全部
 * platform_*.h 都落在 C 链接内，避免 C++ 名字修饰导致链接期
 * undefined reference。本头自身无函数声明，包裹的意义即在于此。
 *
 * 注意 1：平台选择宏 / 能力开关宏仍必须先于聚合包含定义完毕（顺序未变），
 *         故 platform_xxx.h 反向 #include "platform.h" 依旧由 include guard 安全拦截。
 * 注意 2：platform_thread.h / platform_network.h / platform_sleep.h /
 *         platform_sync.h / platform_screencapture.h / platform_audio_capture.h /
 *         platform_websocket.h 自带包裹，这里会出现「extern "C" 嵌套」——
 *         嵌套在 C++ 中是**合法**的，请勿拆掉其中一层。
 * ------------------------------------------------------------------- */
#ifdef __cplusplus
extern "C" {
#endif

#if PLATFORM_USE_THREAD_OBJECT
#  include "platform_thread.h"      /* ThreadObject* NewPlatformThreadObject(PlatformThreadConfig*) */
#endif

#if PLATFORM_USE_MEMORY
#  include "platform_memory.h"      /* PlatformMemoryInterface NewPlatformMemory(void) */
#endif

#if PLATFORM_USE_TIMESTAMP
#  include "platform_timestamp.h"   /* PlatformTimestampInterface NewPlatformTimestamp(void) */
#endif

#if PLATFORM_USE_NETWORK
#  include "platform_network.h"     /* PlatformNetwork* NewPlatformNetworkNative(void) */
#endif

#if PLATFORM_USE_SLEEP
#  include "platform_sleep.h"       /* PlatformSleepInterface NewPlatformSleep(void) */
#endif

#if PLATFORM_USE_SYNC
#  include "platform_sync.h"        /* PlatformMutex* NewPlatformMutexForPlatform(void) */
#endif

#if PLATFORM_USE_SCREENCAPTURE
#  include "platform_screencapture.h" /* PlatformScreenCapture* NewPlatformScreenCaptureNative(void) */
#endif

#if PLATFORM_USE_AUDIO_CAPTURE
#  include "platform_audio_capture.h" /* PlatformAudioCapture* NewPlatformAudioCaptureNative(const PlatformAudioCaptureConfig*) */
#endif

#if PLATFORM_USE_WEBSOCKET
#  include "platform_websocket.h"     /* PlatformWebSocket* NewPlatformWebSocketNative(const PlatformWebSocketConfig*) */
#endif

#ifdef __cplusplus
}
#endif

#endif /* _PLATFORM_H */
