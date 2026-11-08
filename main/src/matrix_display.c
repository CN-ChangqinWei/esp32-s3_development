/* ==================================================================
 * matrix_display.c —— LED 矩阵刷新服务（OOP in C 具象服务）
 *
 * 【线程边界】——本文件最重要的约定
 *
 *   调用线程                              event_loop worker 线程
 *   ------------------------------------  ----------------------------------
 *   MatrixDisplayShow                      matrixDisplayRenderTask
 *     - 写 pendingPattern                    - ScreenWidth / ScreenHeight
 *     - post 渲染任务                        - ScreenClear
 *     - 立即返回                             - 画 1 像素边框
 *                                            - 按 pendingPattern 叠加内容
 *                                            - ScreenFlush
 *
 *   结论：帧缓冲的【写】与下发给硬件的【flush】全部只在 worker 线程内
 *   发生（即 SPI 通讯线程），调用线程从不直接调用 Screen 的任何槽位，
 *   因此不存在「业务线程改帧缓冲、刷新线程同时在发帧」的数据竞争。
 *
 * 【图案几何】——不出现尺寸字面量
 *   屏幕宽高一律来自 ScreenWidth / ScreenHeight；
 *   方块边长与图标边长一律引用 board_config.h 的 BOARD_MATRIX_BLOCK；
 *   边框线宽 1 像素属于图案语义常量（不是矩阵尺寸），单独定义为宏。
 *
 * 【返回值】0 成功；-1 参数非法；其余负值 = event_loop post 的返回值原样上抛。
 * ================================================================== */

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>

#include "matrix_display.h"
#include "screen.h"
#include "bitmap.h"
#include "event_loop.h"
#include "board_config.h"

/* ------------------------------------------------------------------
 * 内存钩子默认实现：外部可在创建对象之前整体替换（依赖反转）
 * ------------------------------------------------------------------ */
void* (*matrixDisplayMalloc)(size_t size) = malloc;
void  (*matrixDisplayFree)(void* p)       = free;

/* ------------------------------------------------------------------
 * 图案语义常量
 *
 * MATRIX_DISPLAY_BORDER_ON   : 边框线宽 1 像素，同时也是单像素点亮值
 * MATRIX_DISPLAY_ICON_SIZE   : 图标边长，取单片驱动板块边长（8 像素）
 * ------------------------------------------------------------------ */
#define MATRIX_DISPLAY_BORDER_ON   (1)
#define MATRIX_DISPLAY_ICON_SIZE   (BOARD_MATRIX_BLOCK)

/* ------------------------------------------------------------------
 * 菱形图标图案：逐行一个字节，最高位对应最左列，
 * 数据长度必须等于 MATRIX_DISPLAY_ICON_SIZE。
 * static const：常量区数据，worker 线程只读，无竞争。
 * ------------------------------------------------------------------ */
static const uint8_t kMatrixDisplayIcon[MATRIX_DISPLAY_ICON_SIZE] = {
    0x18u, 0x24u, 0x42u, 0x81u, 0x81u, 0x42u, 0x24u, 0x18u
};

/* ------------------------------------------------------------------
 * 具象数据成员：只放中性类型与抽象指针，
 * 注入的 screen / loop 归调用方所有，本模块不销毁。
 *
 * pendingPattern 会被「调用线程写、worker 线程读」，
 * 故用 volatile uint32_t 承载枚举值（32 位单次读写，避免半字撕裂）；
 * 任务参数的发布顺序由 post 内部的任务队列保证（先写字段再入队）。
 * ------------------------------------------------------------------ */
struct MatrixDisplay {
    Screen*           screen;          /* 注入：显示抽象，本模块不销毁 */
    EventLoop*        loop;            /* 注入：刷新事件队列，本模块不销毁 */
    volatile uint32_t pendingPattern;  /* 待绘图案（MatrixPattern 的取值） */
};

/* ==================================================================
 * 内部工具：单像素点亮
 *
 * 单点失败（越界或底层不支持）只影响该点，不中断整帧绘制；
 * 整帧的成败由 ScreenClear / ScreenFlush 的返回值体现。
 * ================================================================== */
static void matrixDisplayPlot(Screen* screen, int32_t x, int32_t y)
{
    (void)ScreenSetPixel(screen, x, y, MATRIX_DISPLAY_BORDER_ON);
}

/* ==================================================================
 * 1 像素边框：首末两行整行 + 首末两列整列
 * 四个角点被写两次，写入同一电平值，内存帧幂等，无副作用。
 * ================================================================== */
static void matrixDisplayDrawBorder(Screen* screen, int32_t width, int32_t height)
{
    int32_t x = 0;
    int32_t y = 0;

    /* 上下两条横边：首行与末行整行点亮 */
    for (x = 0; x < width; ++x) {
        matrixDisplayPlot(screen, x, 0);
        matrixDisplayPlot(screen, x, height - 1);
    }

    /* 左右两条竖边：首列与末列整列点亮 */
    for (y = 0; y < height; ++y) {
        matrixDisplayPlot(screen, 0, y);
        matrixDisplayPlot(screen, width - 1, y);
    }
}

/* ==================================================================
 * 中心实心方块：边长取单片板块的一半（即半块见方）
 *
 * 边长先按屏幕宽高夹取，屏幕比方块还小时退化为整屏见方，
 * 因此居中推导得到的原点必为非负，不会写越界坐标。
 * ================================================================== */
static void matrixDisplayDrawSquare(Screen* screen, int32_t width, int32_t height)
{
    const int32_t halfBlock = (int32_t)(BOARD_MATRIX_BLOCK / 2u);
    int32_t       side      = halfBlock;
    int32_t       originX   = 0;
    int32_t       originY   = 0;
    int32_t       row       = 0;
    int32_t       col       = 0;

    if (side > width) {
        side = width;
    }
    if (side > height) {
        side = height;
    }
    if ((side <= 0) || (halfBlock <= 0)) {
        return;
    }

    originX = (width - side) / 2;
    originY = (height - side) / 2;

    for (row = 0; row < side; ++row) {
        for (col = 0; col < side; ++col) {
            matrixDisplayPlot(screen, originX + col, originY + row);
        }
    }
}

/* ==================================================================
 * 菱形图标：经 Bitmap 载体组装，再交给 ScreenDrawBitmap 一次落帧
 *
 * 位图载体是【本函数的临时对象】：画完立即 DeleteBitmap，
 * 不挂到对象上，故多次 Show 之间不共享位图，避免跨线程复用。
 *
 * 位置推导（与既有实现保持一致的视觉位置：居中偏右）：
 *   marginX = (width  - iconSize) / 2   居中时左右两侧的留白
 *   marginY = (height - iconSize) / 2   居中时上下两侧的留白
 *   originX = marginX + marginX / 2     居中基础上再右移半个留白
 *   originY = marginY                   垂直方向保持居中
 * 整数除法保证 originX + iconSize 不超过 width，右 / 下边界不会越界。
 *
 * 失败处理（一律只跳过图标，不阻断本帧）：
 *   屏幕装不下图标、NewBitmap 失败、set 槽位为空，都直接返回，
 *   边框照旧留在帧缓冲里等待 ScreenFlush 上屏。
 * ================================================================== */
static void matrixDisplayDrawIcon(Screen* screen, int32_t width, int32_t height)
{
    const int32_t iconSize = (int32_t)MATRIX_DISPLAY_ICON_SIZE;
    Bitmap*       icon     = NULL;
    int32_t       marginX  = 0;
    int32_t       marginY  = 0;
    int32_t       originX  = 0;
    int32_t       originY  = 0;
    int32_t       row      = 0;
    int32_t       col      = 0;

    /* 屏幕装不下图标：不是装配失败，跳过图标（边框已画好） */
    if ((width < iconSize) || (height < iconSize)) {
        return;
    }

    /* 临时位图载体：只活在本函数内，画完立即释放 */
    icon = NewBitmap((int)MATRIX_DISPLAY_ICON_SIZE, (int)MATRIX_DISPLAY_ICON_SIZE);
    if ((icon == NULL) || (icon->set == NULL)) {
        return;   /* 载体分配不到：跳过图标，边框照刷 */
    }

    /* 把逐行字节图案铺进位图：bit 为 1 的格子置 1，其余保持 0 */
    for (row = 0; row < iconSize; ++row) {
        for (col = 0; col < iconSize; ++col) {
            const uint32_t mask = (uint32_t)1u << (uint32_t)(iconSize - 1 - col);

            if (0u != ((uint32_t)kMatrixDisplayIcon[row] & mask)) {
                (void)icon->set(icon, (int)row, (int)col, (Bit)1);
            }
        }
    }

    marginX = (width - iconSize) / 2;
    marginY = (height - iconSize) / 2;
    originX = marginX + (marginX / 2);
    originY = marginY;

    /* ScreenDrawBitmap 只绘制 bit 为 1 的像素，bit 为 0 视为透明，
     * 因此图标不会擦掉已画的边框，调用顺序无需颠倒。 */
    (void)ScreenDrawBitmap(screen, originX, originY, icon);

    DeleteBitmap(icon);
    icon = NULL;
}

/* ==================================================================
 * 渲染任务：在 event_loop worker 线程内被同步调用
 *
 * 这是本模块【唯一】触碰帧缓冲与 ScreenFlush 的函数。
 * param 固定为 MatrixDisplay 自身（由 Show 在 post 时传入）。
 * ================================================================== */
static void matrixDisplayRenderTask(void* param)
{
    MatrixDisplay* self    = (MatrixDisplay*)param;
    Screen*        screen  = NULL;
    int            width   = 0;
    int            height  = 0;
    uint32_t       pattern = 0u;

    if (self == NULL) {
        return;
    }

    screen = self->screen;
    if (screen == NULL) {
        return;
    }

    /* 几何来自抽象层：任一非正即为无法绘制的屏，直接放弃本帧 */
    width  = ScreenWidth(screen);
    height = ScreenHeight(screen);
    if ((width <= 0) || (height <= 0)) {
        return;
    }

    /* 整帧重建：先清掉上一帧残留，再逐层叠加，避免残影 */
    (void)ScreenClear(screen);

    matrixDisplayDrawBorder(screen, (int32_t)width, (int32_t)height);

    /* volatile 单次读取到局部变量，保证一次渲染只按一个图案走 */
    pattern = self->pendingPattern;
    if (pattern == (uint32_t)MatrixPatternBorderIcon) {
        matrixDisplayDrawIcon(screen, (int32_t)width, (int32_t)height);
    } else {
        matrixDisplayDrawSquare(screen, (int32_t)width, (int32_t)height);
    }

    /* 唯一一次整帧下发：此后 SPI 时序全部在驱动具象内部完成 */
    (void)ScreenFlush(screen);
}

/* ==================================================================
 * 构造：只校验配置 + 保存注入指针，不建线程、不碰硬件、不 post
 * ================================================================== */
MatrixDisplay* NewMatrixDisplay(const MatrixDisplayConfig* config)
{
    MatrixDisplay* self = NULL;

    if (config == NULL) {
        return NULL;
    }
    if ((config->screen == NULL) || (config->loop == NULL)) {
        return NULL;
    }
    if (matrixDisplayMalloc == NULL) {
        return NULL;
    }

    self = (MatrixDisplay*)matrixDisplayMalloc(sizeof(MatrixDisplay));
    if (self == NULL) {
        return NULL;
    }

    self->screen = config->screen;
    self->loop   = config->loop;

    /* 默认图案：边框 + 中心方块（枚举隐式 0 值，首次 Show 之前即为确定状态） */
    self->pendingPattern = (uint32_t)MatrixPatternBorderSquare;

    return self;
}

/* ==================================================================
 * 析构：只释放自身并置空外部指针
 *
 * 注入的 screen / loop 所有权不归本模块，禁止在此调用
 * DeleteScreen / DeleteEventLoop（否则会破坏调用方持有的对象）。
 * ================================================================== */
void DeleteMatrixDisplay(MatrixDisplay** pDisplay)
{
    MatrixDisplay* self = NULL;

    if (pDisplay == NULL) {
        return;
    }

    self = *pDisplay;
    if (self == NULL) {
        return;
    }

    /* 先摘掉外部指针，再清空依赖，最后释放，避免任何悬空访问窗口 */
    *pDisplay = NULL;

    self->screen = NULL;
    self->loop   = NULL;

    if (matrixDisplayFree != NULL) {
        matrixDisplayFree(self);
    }
    self = NULL;
}

/* ==================================================================
 * 对外：请求显示一张图案（异步）
 *
 * 调用线程只做两件事：写 pendingPattern、post 任务，随后立即返回。
 * 全部绘制与 ScreenFlush 都发生在 loop 的 worker 线程内（见文件头说明）。
 * ================================================================== */
int MatrixDisplayShow(MatrixDisplay* display, MatrixPattern pattern)
{
    EventLoop* loop = NULL;
    int        ret  = MATRIX_DISPLAY_OK;

    if (display == NULL) {
        return MATRIX_DISPLAY_ERR_PARAM;
    }
    if ((pattern != MatrixPatternBorderSquare) && (pattern != MatrixPatternBorderIcon)) {
        return MATRIX_DISPLAY_ERR_PARAM;
    }

    loop = display->loop;
    if ((display->screen == NULL) || (loop == NULL)) {
        return MATRIX_DISPLAY_ERR_PARAM;
    }
    if (loop->interfaces.post == NULL) {
        return MATRIX_DISPLAY_ERR_QUEUE;   /* 未实现：入队槽位为空 */
    }

    /* 先记意图再入队：worker 线程取到任务时图案字段一定已可见 */
    display->pendingPattern = (uint32_t)pattern;

    ret = loop->interfaces.post(loop->instance, matrixDisplayRenderTask, display);
    if (ret < 0) {
        return ret;   /* 队列满 / instance 为空 / task 为空：原样上抛 */
    }

    return MATRIX_DISPLAY_OK;
}
