#ifndef _THREAD_OBJECT_WINDOWS_H
#define _THREAD_OBJECT_WINDOWS_H
#include"thread_object.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 【平台隔离 · 重要】本头**不引入任何平台头**（不含 <windows.h> / <process.h>）。
 * 本头会被 C++ 编译单元（Qt 层 / demo，以及 platform_thread.c）包含：一旦把
 * <windows.h> 拉进来，它的 `#define interface struct` 与 IN / OUT / TRUE / FALSE /
 * min / max 等宏会污染整个工程，还可能与本模块形如 `interfaces` 的字段名冲突，
 * 造成 C++ 编译期难以定位的错误（口径与 platform_websocket.h / platform_network.h /
 * platform_audio_capture.h 的「平台头只许出现在 .c」一致）。
 * 因此 Win32 类型在此一律以**等宽基本类型**承载（见下面结构体字段注释），
 * 实际的系统头只在 thread_object_windows.c 内引入。
 *
 * 本具象声明统一带 extern "C" 包裹：保证按 C 链接规范导出，
 * 避免 C++ 名字修饰与 C 实现不匹配（链接期 undefined reference）；
 * 写法与 thread_object_linux.h / thread_object_macos.h 一致。 */
typedef struct ThreadObjectWindowsConfig{
    void* safe;                       /* 安全属性句柄（_beginthreadex 首参）；NULL = 默认 */
    int   stackSize;                  /* 线程栈字节数；0 = 平台默认 */
    void  (*workFunc)(void* param);   /* 线程体（兜底），可为 NULL */
    void  *workParam;                 /* 线程体实参 */
}ThreadObjectWindowsConfig;

/* ThreadObject 在 Windows 下的具象实例。
 * 【ABI】hThread / id **有意**用 void* / unsigned long 承载，而非 HANDLE / DWORD：
 *   - HANDLE 在 Win32 下就是 void*（x86 / x64 均如此，布局完全相同）；
 *   - DWORD  在 Win32 下就是 unsigned long（32 位，布局完全相同）。
 * 故 sizeof 与各字段偏移 / 对齐**全部不变**；.c 内再按 HANDLE / DWORD 的语义使用
 * （需要 HANDLE / DWORD 类型的 API 处做显式转换）。 */
typedef struct ThreadObjectWindows{
    void*         hThread;    /* 线程句柄（.c 内按 HANDLE 用）；NULL = 未创建或已回收 */
    unsigned long id;         /* 系统线程 id（.c 内按 DWORD 用）；日志 / 标识用途 */
    char isRun;
    char isContinue;
    ThreadObjectWindowsConfig config;
    ThreadObjectInterface interfaces;
}ThreadObjectWindows;

ThreadObject* NewThreadObjectWindows(ThreadObjectWindowsConfig* config);
int InitThreadObjectWindows(ThreadObject* obj);

#ifdef __cplusplus
}
#endif
#endif
