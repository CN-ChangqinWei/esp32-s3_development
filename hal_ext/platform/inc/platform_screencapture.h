#ifndef _PLATFORM_SCREENCAPTURE_H
#define _PLATFORM_SCREENCAPTURE_H

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
 * platform_screencapture —— 跨平台「屏幕捕获」能力门面（对象式）
 *
 * 职责：
 *   为「非 UI 层不得使用 Qt」提供纯 C 的屏幕抓取能力，供 core 的 img_fetch 具象
 *   包装使用。调用方拿到的是「屏幕位图字节流」，不感知任何平台 API。
 *
 * 平台差异（由 platform_screencapture.c 的互斥分支选择，头文件不感知平台）：
 *   Windows : GDI 具象（GetDC(NULL) + BitBlt + GetDIBits）；需链接 gdi32 / user32
 *   Linux / macOS / FreeRTOS : 暂无可移植实现，NewPlatformScreenCaptureNative() 返回 NULL
 *                             （保证任何平台都能链接通过）
 *
 * ---------------------------------------------------------------------
 * 输出格式契约（重要，消费方务必遵守）：
 * ---------------------------------------------------------------------
 *   capture() 输出的是 **24 位、无压缩、bottom-up 的 BMP 文件字节流**，即
 *     [ BITMAPFILEHEADER 14 字节 ][ BITMAPINFOHEADER 40 字节 ][ 像素数据 ]
 *   精确布局：
 *     偏移 0x00  BYTE  'B'          偏移 0x01  BYTE  'M'
 *     偏移 0x02  DWORD 文件总字节数（= 54 + 行跨距 * 高）
 *     偏移 0x06  WORD  0（保留）
 *     偏移 0x08  WORD  0（保留）
 *     偏移 0x0A  DWORD 像素数据起始偏移（= 54）
 *     偏移 0x0E  DWORD biSize          = 40
 *     偏移 0x12  LONG  biWidth         = w
 *     偏移 0x16  LONG  biHeight        = h（**正数 = bottom-up**，即首行是图像最下面一行）
 *     偏移 0x1A  WORD  biPlanes        = 1
 *     偏移 0x1C  WORD  biBitCount      = 24
 *     偏移 0x1E  DWORD biCompression   = 0（BI_RGB，无压缩）
 *     偏移 0x22  DWORD biSizeImage     = 行跨距 * h
 *     偏移 0x26  LONG  biXPelsPerMeter = 0
 *     偏移 0x2A  LONG  biYPelsPerMeter = 0
 *     偏移 0x2E  DWORD biClrUsed       = 0
 *     偏移 0x32  DWORD biClrImportant  = 0
 *     偏移 0x36  像素数据：每像素 3 字节，顺序 **B, G, R**；每行按 4 字节对齐
 *                （行跨距 = ((w * 3 + 3) / 4) * 4，不足处补 0 字节）
 *   多字节整数一律 **小端**。
 *   选择 BMP 的理由：无压缩、自描述（宽高在头部，ImgFetch 层无需额外元数据）、
 *   GDI 抓下来的 DIB 天然就是 bottom-up BGR（几乎零转换）、零第三方依赖，
 *   且可被 llama.cpp 多模态栈（stb_image）与常见图像库解码 —— 便于直接送远程视觉模型。
 *
 * 语义约定：
 *   desktopSize —— 返回**虚拟桌面**（所有显示器并集）的宽高；0 成功，<0 失败。
 *   capture     —— 捕获虚拟桌面坐标系下的矩形 (x, y, w, h)。
 *                  x / y **允许为负**（虚拟桌面左上角可能不在 (0,0)）。
 *                  w <= 0 或 h <= 0 一律视为非法入参，返回 <0。
 *                  返回 >0 = BMP 字节数；<0 = 失败。
 *                  *data 指向**本实例内部持有**的缓冲，调用方**只读**，
 *                  有效期至「下一次 capture 或 deleteInstance」为止；
 *                  调用方**不得 free、不得改写**该缓冲。
 *   deleteInstance —— 释放会话与实例（幂等）。
 * ===================================================================== */

typedef struct PlatformScreenCaptureInterface{
    /* 查询虚拟桌面尺寸；0 = 成功；<0 = 失败（width/height 为 NULL 时返回 <0） */
    int  (*desktopSize)(void* instance, int* width, int* height);
    /* 捕获矩形 -> 24 位 BMP 文件字节流（布局见上方契约）；
     * 返回 >0 = 字节数，<0 = 失败；*data 指向实例内部缓冲（只读、不得 free） */
    int  (*capture)(void* instance, int x, int y, int w, int h, void** data);
    /* 释放会话与实例（幂等） */
    void (*deleteInstance)(void* instance);
} PlatformScreenCaptureInterface;

typedef struct PlatformScreenCapture{
    void*                          instance;      /* 指向具体实现实例 */
    PlatformScreenCaptureInterface interfaces;    /* 虚表，字段名固定为 interfaces */
} PlatformScreenCapture;

/* 抽象构造：参数表固定，勿改（只做「分配抽象对象 + 存 instance + 存虚表」三件事） */
PlatformScreenCapture* NewPlatformScreenCapture(void* instance, PlatformScreenCaptureInterface interfaces);
int                    InitPlatformScreenCapture(PlatformScreenCapture* capture, void* instance,
                                                 PlatformScreenCaptureInterface interfaces);
void                   DeletePlatformScreenCapture(PlatformScreenCapture** pCapture);

#if PLATFORM_USE_SCREENCAPTURE
/* 平台中立工厂：Windows 返回 GDI 具象；其余平台返回 NULL（并 log_error）。
 * 本层不带任何默认 ROI / 默认显示器 / 默认格式开关，全部由调用方经 capture() 传参。 */
PlatformScreenCapture* NewPlatformScreenCaptureNative(void);
#endif /* PLATFORM_USE_SCREENCAPTURE */

#ifdef __cplusplus
}
#endif

#endif /* _PLATFORM_SCREENCAPTURE_H */
