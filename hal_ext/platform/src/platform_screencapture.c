#include "platform_screencapture.h"
#include "assert_marco.h"
#include <string.h>
#include <stddef.h>

/* =====================================================================
 * platform_screencapture —— 跨平台「屏幕捕获」能力实现（对象式）
 *
 * 结构（自顶向下）：
 *   1) 能力开关 PLATFORM_USE_SCREENCAPTURE：关闭时不生成任何函数；
 *   2) 抽象层（与平台无关，能力开启即无条件生成）：
 *        NewPlatformScreenCapture / InitPlatformScreenCapture / DeletePlatformScreenCapture；
 *   3) 平台互斥分支（同一编译单元只命中一个）：
 *        PLATFORM_WINDOWS  -> GDI 具象（GetDC/BitBlt/GetDIBits）+ NewPlatformScreenCaptureNative
 *        #else（Linux/macOS/FreeRTOS）-> NewPlatformScreenCaptureNative 返回 NULL（保证可链接）
 *
 * 内存：整个模块的堆分配/释放统一走同模块能力 NewPlatformMemory()
 *       （见 platform_memory.h）；若编译期关闭了内存能力（PLATFORM_USE_MEMORY==0），
 *       为不与本文件的自依赖冲突，退回 <stdlib.h> 的 malloc/free，并在此注明原因。
 *
 * 平台隔离：具体实现结构体（PlatformScreenCaptureWindows）定义在本 .c 的文件作用域内，
 *       绝不写进头文件；同时**严禁**把 <windows.h> 等平台头放进
 *       platform_screencapture.h —— 该头会被 C++ 的 Qt demo 间接包含，一旦引入
 *       <windows.h>，其 COM 头里的 `#define interface struct` 与 IN/OUT/TRUE/FALSE
 *       等宏会污染整个工程，并可能与本头中形如 `interfaces` 的字段名冲突，导致 C++
 *       编译期出现难以定位的错误。这是对「具体实现结构体放头文件」规范的**有意偏离**。
 * ===================================================================== */

#if !PLATFORM_USE_SCREENCAPTURE

/* 能力关闭：不生成任何函数。
 * platform_screencapture.h 中的 typedef 仍然存在（本编译单元非空），无需额外占位。 */

#else /* PLATFORM_USE_SCREENCAPTURE */

/* ---------------------------------------------------------------------
 * 内存：统一走同模块能力 NewPlatformMemory()
 * ------------------------------------------------------------------- */
#if PLATFORM_USE_MEMORY

static void* screencaptureMalloc(size_t size)
{
    PlatformMemoryInterface mem = NewPlatformMemory();
    return mem.malloc(size);
}

static void screencaptureFree(void* ptr)
{
    PlatformMemoryInterface mem = NewPlatformMemory();
    mem.free(ptr);
}

#else /* PLATFORM_USE_MEMORY == 0 */

/* 内存能力被裁剪时 NewPlatformMemory 不可用。本模块必须能分配抽象对象与具体
 * 实例，故按规范允许的例外退回标准库 malloc/free（仅此一处，且为唯一退路）。 */
#include <stdlib.h>

static void* screencaptureMalloc(size_t size)
{
    return malloc(size);
}

static void screencaptureFree(void* ptr)
{
    free(ptr);
}

#endif /* PLATFORM_USE_MEMORY */

/* =====================================================================
 * 抽象层（与平台无关，能力开启即无条件生成）
 * ===================================================================== */

PlatformScreenCapture* NewPlatformScreenCapture(void* instance, PlatformScreenCaptureInterface interfaces)
{
    PlatformScreenCapture* capture;

    ASSERT_NOT_NULL(instance, "NewPlatformScreenCapture: instance is null", NULL);

    capture = (PlatformScreenCapture*)screencaptureMalloc(sizeof(PlatformScreenCapture));
    ASSERT_NOT_NULL(capture, "NewPlatformScreenCapture: screencaptureMalloc failed", NULL);

    /* 交由 Init 完成装配（只设置 instance/interfaces，禁止手工赋值） */
    if (InitPlatformScreenCapture(capture, instance, interfaces) != 0) {
        screencaptureFree(capture);
        capture = NULL;
        return capture;
    }

    return capture;
}

int InitPlatformScreenCapture(PlatformScreenCapture* capture, void* instance,
                              PlatformScreenCaptureInterface interfaces)
{
    ASSERT_NOT_NULL(capture, "InitPlatformScreenCapture: capture is null", -1);
    ASSERT_NOT_NULL(instance, "InitPlatformScreenCapture: instance is null", -2);

    capture->instance   = instance;
    capture->interfaces = interfaces;

    return 0;
}

void DeletePlatformScreenCapture(PlatformScreenCapture** pCapture)
{
    PlatformScreenCapture* capture;

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
    screencaptureFree(capture);

    /* 最后置空调用方指针，防悬空 */
    *pCapture = NULL;
}

/* =====================================================================
 * 平台具象：Windows（GDI BitBlt + GetDIBits）
 * ===================================================================== */
#if defined(PLATFORM_WINDOWS)

#include <windows.h>
#include <limits.h>

/* BMP 布局常量：文件头 14 + 信息头 40 = 像素数据起始偏移 54（与头文件契约一致） */
#define SCREENCAPTURE_BMP_FILE_HEADER_SIZE 14
#define SCREENCAPTURE_BMP_INFO_HEADER_SIZE 40
#define SCREENCAPTURE_BMP_PIXEL_OFFSET     (SCREENCAPTURE_BMP_FILE_HEADER_SIZE + SCREENCAPTURE_BMP_INFO_HEADER_SIZE)

/* 具体实现实例（刻意定义在此文件作用域内，绝不进头文件，理由见文件顶部说明） */
typedef struct PlatformScreenCaptureWindows{
    char* buffer;      /* 内部持有的 BMP 缓冲（screencaptureMalloc 分配，随需扩容） */
    int   bufferCap;   /* 缓冲容量（字节） */
    DWORD lastError;   /* 最近一次失败的错误码（诊断用） */
} PlatformScreenCaptureWindows;

static int  desktopSize(void* instance, int* width, int* height);
static int  capture(void* instance, int x, int y, int w, int h, void** data);
static void deleteInstance(void* instance);

/* 小端写入原语：位图头部一律手工按字节落盘（**不 memcpy SDK 的 BITMAPFILEHEADER
 * 结构体**，以免依赖 SDK 的 pack 设置 / 字段填充差异）。 */
static void writeU16Le(unsigned char* p, unsigned int v)
{
    p[0] = (unsigned char)(v & 0xFFu);
    p[1] = (unsigned char)((v >> 8) & 0xFFu);
}

static void writeU32Le(unsigned char* p, unsigned long v)
{
    p[0] = (unsigned char)(v & 0xFFu);
    p[1] = (unsigned char)((v >> 8) & 0xFFu);
    p[2] = (unsigned char)((v >> 16) & 0xFFu);
    p[3] = (unsigned char)((v >> 24) & 0xFFu);
}

/* 手工写 14 字节 BITMAPFILEHEADER（偏移对照头文件契约 0x00..0x0D） */
static void writeBmpFileHeader(unsigned char* p, unsigned long fileBytes)
{
    p[0] = (unsigned char)'B';                             /* 0x00 */
    p[1] = (unsigned char)'M';                             /* 0x01 */
    writeU32Le(p + 2,  fileBytes);                         /* 0x02 文件总字节数 */
    writeU16Le(p + 6,  0);                                 /* 0x06 保留 */
    writeU16Le(p + 8,  0);                                 /* 0x08 保留 */
    writeU32Le(p + 10, (unsigned long)SCREENCAPTURE_BMP_PIXEL_OFFSET);  /* 0x0A = 54 */
}

/* 查询虚拟桌面（所有显示器并集）尺寸；0 成功，<0 失败 */
static int desktopSize(void* instance, int* width, int* height)
{
    PlatformScreenCaptureWindows* self = (PlatformScreenCaptureWindows*)instance;
    int vx, vy, vw, vh;

    ASSERT_NOT_NULL(instance, "platform_screencapture(WinGDI) desktopSize: instance is null", -1);
    ASSERT_NOT_NULL(width, "platform_screencapture(WinGDI) desktopSize: width is null", -2);
    ASSERT_NOT_NULL(height, "platform_screencapture(WinGDI) desktopSize: height is null", -3);

    /* 虚拟桌面左上角 (x, y) 可能为负（副屏在主屏左侧 / 上方），宽高恒 >= 0 */
    vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    if (vw <= 0 || vh <= 0) {
        self->lastError = GetLastError();
        log_error("platform_screencapture(WinGDI) desktopSize: invalid virtual screen origin=(%d,%d) "
                  "size=(%d,%d) (SM_C?VIRTUALSCREEN returned non-positive; no display attached?)",
                  vx, vy, vw, vh);
        return -4;
    }

    *width  = vw;
    *height = vh;

    return 0;
}

/* 捕获虚拟桌面坐标下的矩形 (x, y, w, h) -> 24 位 bottom-up BMP 字节流。
 * 返回 >0 = BMP 字节数；<0 = 失败；*data 指向实例内部缓冲（只读，不得 free）。 */
static int capture(void* instance, int x, int y, int w, int h, void** data)
{
    PlatformScreenCaptureWindows* self = (PlatformScreenCaptureWindows*)instance;
    HDC        hdcScreen = NULL;
    HDC        hdcMem    = NULL;
    HBITMAP    hbm       = NULL;
    HGDIOBJ    hOld      = NULL;
    BITMAPINFO bmi;
    long long  stride;
    long long  pixelBytes;
    long long  fileBytes;
    int        result = -1;

    ASSERT_NOT_NULL(instance, "platform_screencapture(WinGDI) capture: instance is null", -1);
    ASSERT_NOT_NULL(data, "platform_screencapture(WinGDI) capture: data is null", -2);

    *data = NULL;

    /* ① 入参校验：w/h 必须为正（x/y 允许为负，虚拟桌面原点可能不在 (0,0)） */
    if (w <= 0 || h <= 0) {
        log_error("platform_screencapture(WinGDI) capture: invalid roi x=%d y=%d w=%d h=%d "
                  "(require w>0 and h>0)", x, y, w, h);
        return -3;
    }

    /* ② 尺寸计算 + 溢出保护：
     *    行跨距 = ((w*3 + 3) / 4) * 4（4 字节对齐）。全程用 long long 计算，
     *    并用「stride > LLONG_MAX / h」反算判溢出，避免 int 乘法回绕成一个
     *    “看着合法”的小值后越界写缓冲。 */
    stride = (((long long)w * 3) + 3) / 4 * 4;
    if (stride > (long long)LLONG_MAX / (long long)h) {
        log_error("platform_screencapture(WinGDI) capture: pixel size overflow x=%d y=%d w=%d h=%d "
                  "stride=%lld (ROI too large)", x, y, w, h, (long long)stride);
        return -4;
    }
    pixelBytes = stride * (long long)h;
    fileBytes  = (long long)SCREENCAPTURE_BMP_PIXEL_OFFSET + pixelBytes;

    /* capture() 的返回值为 int，位图超过 INT_MAX 无法表达：提前失败并提示缩小 ROI，
     * 绝不把 (int) 截断后的错误长度返回（那会让调用方按错误长度读缓冲）。 */
    if (fileBytes > (long long)INT_MAX) {
        log_error("platform_screencapture(WinGDI) capture: bitmap too large x=%d y=%d w=%d h=%d "
                  "bytes=%lld exceeds INT_MAX=%d (shrink ROI)",
                  x, y, w, h, (long long)fileBytes, (int)INT_MAX);
        return -5;
    }

    /* ③ 内部缓冲随需扩容：新分配成功后再释放旧缓冲（分配失败保持旧缓冲不动） */
    if (fileBytes > (long long)self->bufferCap) {
        char* newBuffer = (char*)screencaptureMalloc((size_t)fileBytes);

        if (newBuffer == NULL) {
            log_error("platform_screencapture(WinGDI) capture: screencaptureMalloc failed "
                      "x=%d y=%d w=%d h=%d need=%lld bytes", x, y, w, h, (long long)fileBytes);
            return -6;
        }
        if (self->buffer != NULL) {
            screencaptureFree(self->buffer);
        }
        self->buffer    = newBuffer;
        self->bufferCap = (int)fileBytes;
    }

    /* ④a 14 字节 BITMAPFILEHEADER：手工按字节小端写（偏移 0x00..0x0D） */
    writeBmpFileHeader((unsigned char*)self->buffer, (unsigned long)fileBytes);

    /* ④b 40 字节 BITMAPINFOHEADER：BITMAPINFOHEADER 无填充，逐字段填好后整块拷入
     *     （偏移 0x0E..0x35；禁用 SDK 的 BITMAPFILEHEADER 结构体，理由见 writeU32Le 注释）。
     *     biHeight 取正数 -> bottom-up，与 BMP 契约一致，无需额外上下翻转。 */
    memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader.biSize          = (DWORD)sizeof(BITMAPINFOHEADER);  /* 0x0E = 40 */
    bmi.bmiHeader.biWidth         = (LONG)w;                          /* 0x12 = w */
    bmi.bmiHeader.biHeight        = (LONG)h;                          /* 0x16 = h（正数 = bottom-up） */
    bmi.bmiHeader.biPlanes        = 1;                                /* 0x1A = 1 */
    bmi.bmiHeader.biBitCount      = 24;                               /* 0x1C = 24 */
    bmi.bmiHeader.biCompression   = BI_RGB;                           /* 0x1E = 0（无压缩） */
    bmi.bmiHeader.biSizeImage     = (DWORD)pixelBytes;                /* 0x22 = stride * h */
    bmi.bmiHeader.biXPelsPerMeter = 0;                                /* 0x26 */
    bmi.bmiHeader.biYPelsPerMeter = 0;                                /* 0x2A */
    bmi.bmiHeader.biClrUsed       = 0;                                /* 0x2E */
    bmi.bmiHeader.biClrImportant  = 0;                                /* 0x32 */
    memcpy(self->buffer + SCREENCAPTURE_BMP_FILE_HEADER_SIZE,
           &bmi.bmiHeader, (size_t)sizeof(BITMAPINFOHEADER));

    /* ⑤ GDI 抓图：屏幕 DC -> 兼容内存 DC -> 兼容位图 -> BitBlt 到内存 DC */
    hdcScreen = GetDC(NULL);
    if (hdcScreen == NULL) {
        self->lastError = GetLastError();
        log_error("platform_screencapture(WinGDI) capture: GetDC(NULL) failed err=%lu x=%d y=%d w=%d h=%d",
                  (unsigned long)self->lastError, x, y, w, h);
        result = -7;
        goto cleanup;
    }

    hdcMem = CreateCompatibleDC(hdcScreen);
    if (hdcMem == NULL) {
        self->lastError = GetLastError();
        log_error("platform_screencapture(WinGDI) capture: CreateCompatibleDC failed err=%lu "
                  "x=%d y=%d w=%d h=%d", (unsigned long)self->lastError, x, y, w, h);
        result = -8;
        goto cleanup;
    }

    hbm = CreateCompatibleBitmap(hdcScreen, w, h);
    if (hbm == NULL) {
        self->lastError = GetLastError();
        log_error("platform_screencapture(WinGDI) capture: CreateCompatibleBitmap failed err=%lu "
                  "x=%d y=%d w=%d h=%d", (unsigned long)self->lastError, x, y, w, h);
        result = -9;
        goto cleanup;
    }

    hOld = SelectObject(hdcMem, hbm);
    if (hOld == NULL || hOld == HGDI_ERROR) {
        self->lastError = GetLastError();
        log_error("platform_screencapture(WinGDI) capture: SelectObject failed err=%lu x=%d y=%d w=%d h=%d",
                  (unsigned long)self->lastError, x, y, w, h);
        hOld   = NULL;      /* 未成功选入：cleanup 不再尝试恢复 */
        result = -10;
        goto cleanup;
    }

    /* CAPTUREBLT 让分层窗口（layered window）也一并被抓进内存 DC */
    if (!BitBlt(hdcMem, 0, 0, w, h, hdcScreen, x, y, SRCCOPY | CAPTUREBLT)) {
        self->lastError = GetLastError();
        log_error("platform_screencapture(WinGDI) capture: BitBlt failed err=%lu x=%d y=%d w=%d h=%d",
                  (unsigned long)self->lastError, x, y, w, h);
        result = -11;
        goto cleanup;
    }

    /* ⑥ GetDIBits：把位图读成 24bpp bottom-up BGR，直接写进 BMP 像素区（buffer + 54）。
     *    biHeight 取正 -> bottom-up，与 BMP 契约一致。 */
    if (GetDIBits(hdcMem, hbm, 0, (UINT)h,
                  self->buffer + SCREENCAPTURE_BMP_PIXEL_OFFSET,
                  &bmi, DIB_RGB_COLORS) == 0) {
        self->lastError = GetLastError();
        log_error("platform_screencapture(WinGDI) capture: GetDIBits failed err=%lu x=%d y=%d w=%d h=%d",
                  (unsigned long)self->lastError, x, y, w, h);
        result = -12;
        goto cleanup;
    }

    /* ⑧ 成功：交出内部缓冲与字节数 */
    *data  = self->buffer;
    result = (int)fileBytes;

cleanup:
    /* ⑦ 清理：无论成功失败都执行，顺序固定 ——
     *   1) SelectObject(hdcMem, hOld) 把 hbm 从 DC 摘出并恢复原图；
     *   2) DeleteObject(hbm)          删位图；
     *   3) DeleteDC(hdcMem)           删内存 DC；
     *   4) ReleaseDC(NULL, hdcScreen) 释放屏幕 DC。
     * 每步用前判非空，用完置零，避免二次释放。 */
    if (hdcMem != NULL) {
        if (hOld != NULL) {
            SelectObject(hdcMem, hOld);     /* 恢复原图（hbm 已选入时必做，否则 DeleteObject 不生效） */
        }
        if (hbm != NULL) {
            DeleteObject(hbm);
        }
        DeleteDC(hdcMem);
    }
    if (hdcScreen != NULL) {
        ReleaseDC(NULL, hdcScreen);
    }
    hOld      = NULL;
    hbm       = NULL;
    hdcMem    = NULL;
    hdcScreen = NULL;

    if (result < 0) {
        *data = NULL;   /* 失败：绝不把半成品缓冲交给调用方 */
    }

    return result;
}

/* 释放会话与实例（幂等） */
static void deleteInstance(void* instance)
{
    PlatformScreenCaptureWindows* self;

    if (instance == NULL) {
        return;
    }
    self = (PlatformScreenCaptureWindows*)instance;

    if (self->buffer != NULL) {
        screencaptureFree(self->buffer);
        self->buffer = NULL;
    }
    self->bufferCap = 0;

    /* 销毁具体实例（抽象包装器由 DeletePlatformScreenCapture 释放） */
    screencaptureFree(self);
}

/* 组装 Windows 具象虚表 */
static PlatformScreenCaptureInterface windowsInterface(void)
{
    PlatformScreenCaptureInterface interfaces;

    interfaces.desktopSize    = desktopSize;
    interfaces.capture        = capture;
    interfaces.deleteInstance = deleteInstance;

    return interfaces;
}

/* Windows 具体构造：装配具体实例 + 调抽象构造完成包装（禁止手工赋值） */
static PlatformScreenCapture* NewPlatformScreenCaptureWindows(void)
{
    PlatformScreenCaptureWindows* self;
    PlatformScreenCapture*        capture;

    self = (PlatformScreenCaptureWindows*)screencaptureMalloc(sizeof(PlatformScreenCaptureWindows));
    ASSERT_NOT_NULL(self, "NewPlatformScreenCaptureWindows: screencaptureMalloc failed", NULL);
    memset(self, 0, sizeof(PlatformScreenCaptureWindows));

    capture = NewPlatformScreenCapture(self, windowsInterface());
    if (capture == NULL) {
        screencaptureFree(self);
        self = NULL;
        return capture;
    }

    return capture;
}

/* 平台中立工厂：Windows -> GDI 具象 */
PlatformScreenCapture* NewPlatformScreenCaptureNative(void)
{
    return NewPlatformScreenCaptureWindows();
}

/* =====================================================================
 * 其它平台（Linux / macOS / FreeRTOS）：暂无可移植具象
 * ===================================================================== */
#else

/* 保证任何平台都能链接通过：工厂返回 NULL（并 log_error）。
 * 抽象构造 / DeletePlatformScreenCapture 仍然可用（上层可自行注入自备的具体实例）。 */
PlatformScreenCapture* NewPlatformScreenCaptureNative(void)
{
    log_error("NewPlatformScreenCaptureNative: screen capture not available on this platform");
    return NULL;
}

#endif /* 平台互斥分支 */

#endif /* PLATFORM_USE_SCREENCAPTURE */
