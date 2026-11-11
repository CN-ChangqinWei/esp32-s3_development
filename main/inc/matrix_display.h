#ifndef _MATRIX_DISPLAY_H
#define _MATRIX_DISPLAY_H

/* ==================================================================
 * matrix_display.h —— LED 矩阵刷新服务（刷新事件走 event_loop）
 *
 * 职责（只做一件事）：
 *   把「画哪一张图案」这一意图包装成 event_loop 任务，
 *   由 event_loop 的 worker 线程完成全部绘制与 ScreenFlush。
 *
 * 线程边界（本模块的立身之本）：
 *   - 调用线程（业务线程 / 初始化线程）：只写 pendingPattern + post，
 *     立即返回，绝不触碰屏幕帧缓冲。
 *   - event_loop worker 线程（其构造函数注入的 threadObj 所属线程，
 *     即 SPI 通讯线程）：执行 matrixDisplayRenderTask，在其内部完成
 *     获取宽高、清屏、画边框、画图案、ScreenFlush。
 *   这样帧缓冲的读写与 SPI 下发永远只发生在同一个线程，
 *   不会与刷新线程并发修改同一块像素内存。
 *
 * 依赖方向（严格单向，只依赖抽象）：
 *   screen.h      Screen 抽象父类（提供像素与 flush 能力）
 *   event_loop.h  EventLoop 抽象父类（提供跨线程任务队列）
 *   不包含任何平台头（freertos / driver / esp_*.h），也不含引脚与寄存器。
 *
 * 所有权约定：
 *   config 里的 screen / loop 由调用方创建并拥有，本模块【只借用】，
 *   构造时不新建线程，析构时也不删除这两个注入对象。
 *
 * 返回值约定：
 *     0   成功
 *    -1   参数非法（对象为空、注入依赖为空、pattern 越界）
 *    -2   未实现 / 队列满（post 槽位为空或入队失败；post 的负返回值原样上抛）
 * ================================================================== */

#include <stdint.h>

#include "screen.h"
#include "event_loop.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------
 * 本模块内存钩子：默认指向 malloc / free，
 * 外部可在创建对象之前替换为平台专用分配器（依赖反转）：
 *     matrixDisplayMalloc = pvPortMalloc;   matrixDisplayFree = vPortFree;
 * 仅用于本对象自身，注入的 Screen / EventLoop 内存不归本模块管理。
 * ------------------------------------------------------------------ */
extern void* (*matrixDisplayMalloc)(size_t size);
extern void  (*matrixDisplayFree)(void* p);

/* ------------------------------------------------------------------
 * 返回值约定宏（0 成功、负值失败，与 hal_ext 各模块口径一致）
 * ------------------------------------------------------------------ */
#define MATRIX_DISPLAY_OK               0
#define MATRIX_DISPLAY_ERR_PARAM      (-1)
#define MATRIX_DISPLAY_ERR_QUEUE      (-2)

/* ------------------------------------------------------------------
 * 具象对象：字段不对外暴露（不完整类型），
 * 使用方一律以 MatrixDisplay* 句柄操作。
 * ------------------------------------------------------------------ */
typedef struct MatrixDisplay MatrixDisplay;

/* ------------------------------------------------------------------
 * 图案枚举
 *
 *   BorderSquare  边框 + 中心实心方块（纯 setPixel 组合，零依赖）
 *   BorderIcon    边框 + 菱形图标（经 Bitmap 载体 + ScreenDrawBitmap 落帧）
 *   AllOn         全屏点亮（LED 全亮测试：不画边框与图标，
 *                 整屏一次 ScreenFill 置亮后 ScreenFlush）
 *   AllOff        全屏熄灭（LED 全灭测试：不画边框与图标，
 *                 整屏一次 ScreenFill 置零后 ScreenFlush）
 *
 * 前两种图案都先画 1 像素边框，差别只在中部内容；
 * AllOn / AllOff 例外：整屏填充（全亮 / 全灭），与边框 / 图标互斥。
 * ------------------------------------------------------------------ */
typedef enum MatrixPattern {
    MatrixPatternBorderSquare = 0,
    MatrixPatternBorderIcon   = 1,
    MatrixPatternAllOn        = 2,   /* 全屏点亮（LED 全亮测试） */
    MatrixPatternAllOff       = 3    /* 全屏熄灭（LED 全灭测试） */
} MatrixPattern;

/* ------------------------------------------------------------------
 * 构造配置：外部依赖注入
 *
 *   screen : 显示抽象，本模块只调用其虚表，不销毁
 *   loop   : 刷新事件队列，其 worker 线程即 SPI 通讯线程；本模块不启动、
 *            不停止、不销毁该循环，只 post 任务
 * 两者任一为空则构造失败。
 * ------------------------------------------------------------------ */
typedef struct MatrixDisplayConfig {
    Screen*    screen;
    EventLoop* loop;
} MatrixDisplayConfig;

/* ------------------------------------------------------------------
 * 构造 / 析构
 *
 * NewMatrixDisplay：
 *   只校验配置并保存注入指针，默认待绘图案为 BorderSquare；
 *   不创建线程、不操作硬件、不 post 任何任务（首次上屏由调用方 Show 触发）。
 *   失败（config 为空、screen 为空、loop 为空、内存不足）返回 NULL。
 *
 * DeleteMatrixDisplay：
 *   只释放本对象自身并把外部指针置空；注入的 screen / loop 不在此析构。
 *   允许传入 NULL 或指向 NULL 的指针（空操作）。
 * ------------------------------------------------------------------ */
MatrixDisplay* NewMatrixDisplay(const MatrixDisplayConfig* config);
void           DeleteMatrixDisplay(MatrixDisplay** pDisplay);

/* ------------------------------------------------------------------
 * 请求显示一张图案（异步）
 *
 * 动作：
 *   1) 记录待绘图案到对象内部
 *   2) 向 event_loop post 渲染任务，随后立即返回
 * 真正清屏 / 画边框 / 画图案 / flush 全部在 loop 的 worker 线程内发生。
 *
 * 同一对象连续 Show 多次时，按入队顺序逐帧渲染；
 * 队列满时本次请求失败（不排队、不覆盖正在渲染的帧）。
 *
 * 返回：0 成功（已入队）；-1 参数非法；其余负值 = post 的返回值原样上抛。
 * ------------------------------------------------------------------ */
int            MatrixDisplayShow(MatrixDisplay* display, MatrixPattern pattern);

#ifdef __cplusplus
}
#endif

#endif /* _MATRIX_DISPLAY_H */
