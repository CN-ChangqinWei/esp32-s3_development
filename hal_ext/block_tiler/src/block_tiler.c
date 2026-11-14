/* ==================================================================
 * block_tiler.c —— 8x8 位图平铺自检服务（OOP in C 具象服务）
 *
 * 【线程边界】——本文件最重要的约定
 *
 *   调用线程                                event_loop worker 线程
 *   --------------------------------------  ------------------------------------
 *   BlockTilerShowLetterT / ShowGlyph        blockTilerRenderTask
 *     - 准备并写入对象内部的 glyph 缓冲        - ScreenClear
 *     - post 渲染任务                         - 双重循环 cy / cx 逐块绘制
 *     - 立即返回                              - ScreenFlush
 *
 *   调用线程从不直接调用 Screen 的任何槽位；帧缓冲的写入与下发硬件的
 *   flush 全部只在注入的 EventLoop 的 worker 线程内发生，因此不存在
 *   「业务线程改帧缓冲、刷新线程同时在发帧」的数据竞争。
 *
 *   glyph 的可见性：调用方先写满 glyph 缓冲、再 post；post 在 EventLoop
 *   内部的互斥量保护下入队，worker 也在同一把锁下出队，二者构成
 *   释放-获取配对，故 worker 取到任务时一定看得见本次写入的内容。
 *   若两个线程并发调用 Show 系列，最终呈现为其中一个字形（后入队者
 *   覆盖先入队者），对自检用途完全可接受，故不额外加锁。
 *
 * 【坐标约定】——与 ScreenSetPixel 完全一致
 *   屏幕原点在左上角，x 向右递增，y 向下递增。
 *   屏幕被等分为 chipsX 列 x chipsY 行个分块，第 (cx, cy) 块的左上角
 *   像素坐标为 (cx * blockW, cy * blockH)。
 *   字形 / 位图的 row = 0 对应块的最上面一行，col = 0 对应块的最左列。
 *
 * 【字母 T 如何对应左上角坐标系】
 *   kBlockTilerLetterT[0] = 0xFF 是【顶横】，画在块的 y = 0 这一行；
 *   其后 7 行的 0x18 是【居中竖笔】（bit7 为最左列，0x18 点亮第 2、3 列），
 *   自 y = 1 一路向下延伸到块的底边。
 *   于是：若观察到的 T 顶横在【上】、竖笔向下，说明 y 轴方向正确；
 *   若顶横跑到底部，说明纵向被翻转（y 轴取反）；
 *   若竖笔偏到一侧、横笔变成竖线，说明 x / y 被交换（转置）。
 *   单块边长小于 8 时只画左上角 blockW x blockH 区域，顶横与竖笔
 *   仍在块的左上角方向呈现，方向判读结论不变。
 *
 * 【几何来源】——不出现屏幕尺寸字面量
 *   宽高一律来自 ScreenWidth / ScreenHeight；单块边长由「能同时整除宽高的
 *   最大边长」推导，并以字形边长（8）为上限。8 只以宏的形式出现，
 *   含义是「字形行数 / 列数 = 单块边长上限」，不代表任何具体屏幕尺寸。
 *
 * 【返回值】0 成功（已入队）；-1 参数非法；-2 post 槽位为空；
 *   其余负值 = event_loop post 的返回值原样上抛。
 * ================================================================== */

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "block_tiler.h"
#include "screen.h"
#include "bitmap.h"
#include "event_loop.h"

/* ------------------------------------------------------------------
 * 内存钩子默认实现：外部可在创建对象之前整体替换（依赖反转）
 * 仅用于本对象自身，注入的 Screen / EventLoop / Bitmap 不归本模块管理。
 * ------------------------------------------------------------------ */
void* (*blockTilerMalloc)(size_t size) = malloc;
void  (*blockTilerFree)(void* p)       = free;

/* ------------------------------------------------------------------
 * 字形尺寸常量
 *
 *   BLOCK_TILER_GLYPH_ROWS / COLS : 字形（也是位图入参）的行数 / 列数，
 *                                   同时是【单块边长的上限】。
 *   BLOCK_TILER_BLOCK_MAX         : 单块边长上限，语义等同字形边长。
 *   BLOCK_TILER_PIXEL_ON          : 点亮值（Screen 语义：非 0 即亮）。
 * ------------------------------------------------------------------ */
#define BLOCK_TILER_GLYPH_ROWS   (8u)
#define BLOCK_TILER_GLYPH_COLS   (8u)
#define BLOCK_TILER_BLOCK_MAX    (BLOCK_TILER_GLYPH_ROWS)
#define BLOCK_TILER_PIXEL_ON     (1)

/* ------------------------------------------------------------------
 * 字母 T 的 8 x 8 字形：行优先，最高位对应最左列
 *   bit 为 1 -> 点亮；为 0 -> 不画（保持清屏后的背景）
 * static const：常量区只读数据，worker 线程只读，无竞争。
 * ------------------------------------------------------------------ */
static const uint8_t kBlockTilerLetterT[BLOCK_TILER_GLYPH_ROWS] = {
    0xFFu,  /* ######## 顶横 */
    0x18u,  /* ...##... 竖笔起点 */
    0x18u,
    0x18u,
    0x18u,
    0x18u,
    0x18u,
    0x18u   /* ...##... 竖笔终点，便于判断上下 / 左右是否翻转 */
};

/* ------------------------------------------------------------------
 * 具象数据成员：只放中性类型与抽象指针
 *
 *   screen / loop : 注入依赖，调用方所有，本模块不销毁
 *   glyph         : 当前待平铺的字形（行优先，最高位 = 最左列）
 *   blockW/blockH : 单个分块的像素宽 / 高（构造时推导，二者相等）
 *   chipsX/chipsY : 横向 / 纵向分块数
 * ------------------------------------------------------------------ */
struct BlockTiler {
    Screen*    screen;                          /* 注入：显示抽象，不销毁 */
    EventLoop* loop;                            /* 注入：渲染队列，不销毁 */
    uint8_t    glyph[BLOCK_TILER_GLYPH_ROWS];   /* 待平铺字形（行优先） */
    uint32_t   blockW;                          /* 单块像素宽 */
    uint32_t   blockH;                          /* 单块像素高（= blockW） */
    uint32_t   chipsX;                          /* 横向分块数 */
    uint32_t   chipsY;                          /* 纵向分块数 */
};

/* ------------------------------------------------------------------
 * 内部函数声明（定义在下方；渲染任务先声明，供 Show 系列引用）
 * ------------------------------------------------------------------ */
static void     blockTilerRenderTask(void* param);
static uint32_t blockTilerResolveBlockEdge(uint32_t width, uint32_t height);
static int      blockTilerResolveGeometry(BlockTiler* self);
static int      blockTilerPackGlyph(const Bitmap* glyph,
                                   uint8_t* outRows);
static int      blockTilerPostRender(BlockTiler* self);

/* ==================================================================
 * 单块边长推导：取能同时整除宽高的【最大】边长，且不超过字形边长
 *
 * 说明：
 *   1. 候选从 min(字形边长, 屏幕宽, 屏幕高) 开始向下试，
 *      第一个能同时整除宽高的值即为结果；
 *   2. 边长 1 恒能整除，故宽高均为正时必能求出结果（最差退化为 1 像素块）；
 *   3. 返回 0 表示入参非法（宽或高为 0）。
 * ================================================================== */
static uint32_t blockTilerResolveBlockEdge(uint32_t width, uint32_t height)
{
    uint32_t edge = BLOCK_TILER_BLOCK_MAX;

    if (width == 0u || height == 0u) {
        return 0u;
    }
    if (width < edge) {
        edge = width;
    }
    if (height < edge) {
        edge = height;
    }

    while (edge > 0u) {
        if (((width % edge) == 0u) && ((height % edge) == 0u)) {
            return edge;
        }
        --edge;
    }

    return 0u;
}

/* ==================================================================
 * 几何推导与校验：宽高取自 Screen 抽象，结果写回对象
 *
 * 校验项（任一不满足即失败）：
 *   - 屏幕宽高均为正（ScreenWidth / ScreenHeight 返回负值表示不可用）
 *   - 能推出非 0 的单块边长，且不超过字形边长上限
 *   - blockW == blockH（本模块只支持方形分块）
 *   - chipsX >= 1 且 chipsY >= 1
 * ================================================================== */
static int blockTilerResolveGeometry(BlockTiler* self)
{
    int32_t  width  = 0;
    int32_t  height = 0;
    uint32_t edge   = 0u;

    if (self == NULL || self->screen == NULL) {
        return BLOCK_TILER_ERR_PARAM;
    }

    width  = ScreenWidth(self->screen);
    height = ScreenHeight(self->screen);
    if ((width <= 0) || (height <= 0)) {
        /* 屏幕尺寸不可用：无法划分分块，构造失败 */
        return BLOCK_TILER_ERR_PARAM;
    }

    edge = blockTilerResolveBlockEdge((uint32_t)width, (uint32_t)height);
    if (edge == 0u) {
        return BLOCK_TILER_ERR_PARAM;
    }
    if (edge > BLOCK_TILER_BLOCK_MAX) {
        return BLOCK_TILER_ERR_PARAM;   /* 防御：单块边长不得超过字形边长 */
    }

    self->blockW = edge;
    self->blockH = edge;
    self->chipsX = (uint32_t)width / edge;
    self->chipsY = (uint32_t)height / edge;

    ASSERT_EQ(self->blockW, self->blockH, "blockTiler: blockW must equal blockH",
              BLOCK_TILER_ERR_PARAM);
    ASSERT_GREATER(self->chipsX, 0u, "blockTiler: chipsX must be at least 1",
                   BLOCK_TILER_ERR_PARAM);
    ASSERT_GREATER(self->chipsY, 0u, "blockTiler: chipsY must be at least 1",
                   BLOCK_TILER_ERR_PARAM);

    return BLOCK_TILER_OK;
}

/* ==================================================================
 * 位图 -> 内部字形缓冲（行优先，每行一个字节，最高位 = 最左列）
 *
 * 归一化规则：
 *   - 位图第 col 列固定落到 bit 位 (字形列数 - 1 - col)，与位图宽高无关；
 *   - 位图窄于字形时靠左对齐，未覆盖的低位补 0；
 *   - 位图矮于字形时下方各行补 0；
 *   - 只认位图 at 槽位返回 1 为点亮（bitmap 契约：1 亮、0 灭、-1 越界）。
 *
 * outRows 必须指向至少 BLOCK_TILER_GLYPH_ROWS 字节的缓冲。
 * Bitmap 接口的 at 槽位原型不带 const，本模块只读访问，故就地转换指针。
 * ================================================================== */
static int blockTilerPackGlyph(const Bitmap* glyph, uint8_t* outRows)
{
    Bitmap*  bmp  = NULL;
    uint32_t rows = 0u;
    uint32_t cols = 0u;
    uint32_t row  = 0u;
    uint32_t col  = 0u;

    ASSERT_NOT_NULL(glyph, "blockTiler packGlyph: glyph is null", BLOCK_TILER_ERR_PARAM);
    ASSERT_NOT_NULL(outRows, "blockTiler packGlyph: outRows is null", BLOCK_TILER_ERR_PARAM);
    ASSERT_NOT_NULL(glyph->at, "blockTiler packGlyph: glyph->at is null", BLOCK_TILER_ERR_PARAM);

    if ((glyph->width <= 0) || (glyph->height <= 0)) {
        return BLOCK_TILER_ERR_PARAM;
    }
    if (((uint32_t)glyph->width > BLOCK_TILER_GLYPH_COLS) ||
        ((uint32_t)glyph->height > BLOCK_TILER_GLYPH_ROWS)) {
        return BLOCK_TILER_ERR_PARAM;   /* 超出单块边长上限，无法平铺 */
    }

    rows = (uint32_t)glyph->height;
    cols = (uint32_t)glyph->width;
    bmp  = (Bitmap*)glyph;

    /* 先整体清零：矮于字形的行、窄于字形的低位自然为 0 */
    (void)memset(outRows, 0, (size_t)BLOCK_TILER_GLYPH_ROWS);

    for (row = 0u; row < rows; ++row) {
        uint8_t bits = 0u;

        for (col = 0u; col < cols; ++col) {
            if (bmp->at(bmp, (int)row, (int)col) == (Bit)1) {
                bits |= (uint8_t)(1u << (BLOCK_TILER_GLYPH_COLS - 1u - col));
            }
        }
        outRows[row] = bits;
    }

    return BLOCK_TILER_OK;
}

/* ==================================================================
 * 渲染任务：在 event_loop 的 worker 线程内执行
 *
 * 流程：清屏 -> 遍历每个分块 -> 逐像素点亮 -> 刷屏。
 *   1. 单像素写入越界时 ScreenSetPixel 返回负值，此处刻意忽略：
 *      一个点写不进去不影响其余像素，整帧成败由 clear / flush 体现；
 *   2. 分块原点由 cx * blockW / cy * blockH 推导，恒落在屏幕内
 *      （blockW = width / chipsX 保证边界不溢出）；
 *   3. 字形行下标 row 恒小于 blockH，而 blockH 不超过字形行数，
 *      故 glyph[row] 永不越界。
 * ================================================================== */
static void blockTilerRenderTask(void* param)
{
    BlockTiler* self   = (BlockTiler*)param;
    Screen*     screen = NULL;
    uint32_t    cy     = 0u;
    uint32_t    cx     = 0u;
    uint32_t    row    = 0u;
    uint32_t    col    = 0u;

    if (self == NULL) {
        return;
    }
    screen = self->screen;
    if (screen == NULL) {
        return;
    }

    (void)ScreenClear(screen);

    for (cy = 0u; cy < self->chipsY; ++cy) {
        for (cx = 0u; cx < self->chipsX; ++cx) {
            const int32_t originX = (int32_t)(cx * self->blockW);
            const int32_t originY = (int32_t)(cy * self->blockH);

            for (row = 0u; row < self->blockH; ++row) {
                const uint8_t bits = self->glyph[row];

                for (col = 0u; col < self->blockW; ++col) {
                    if (((int)bits & (int)(1u << (BLOCK_TILER_GLYPH_COLS - 1u - col))) == 0) {
                        continue;   /* bit 为 0：不画，保持清屏后的背景 */
                    }
                    (void)ScreenSetPixel(screen,
                                         originX + (int32_t)col,
                                         originY + (int32_t)row,
                                         BLOCK_TILER_PIXEL_ON);
                }
            }
        }
    }

    (void)ScreenFlush(screen);
}

/* ==================================================================
 * 入队渲染任务：唯一的跨线程动作
 *
 * 前置条件：self->glyph 已写好（调用方必须先写缓冲、后入队）。
 * 返回 post 的返回值原样，0 成功、负值失败。
 * ================================================================== */
static int blockTilerPostRender(BlockTiler* self)
{
    EventLoop* loop = NULL;
    int        ret  = BLOCK_TILER_OK;

    if (self == NULL) {
        return BLOCK_TILER_ERR_PARAM;
    }
    loop = self->loop;
    if (loop == NULL || self->screen == NULL) {
        return BLOCK_TILER_ERR_PARAM;
    }
    if (loop->interfaces.post == NULL) {
        return BLOCK_TILER_ERR_QUEUE;   /* 未实现：入队槽位为空 */
    }

    ret = loop->interfaces.post(loop->instance, blockTilerRenderTask, (void*)self);
    if (ret < 0) {
        return ret;   /* 队列满 / instance 为空 / task 为空：原样上抛 */
    }

    return BLOCK_TILER_OK;
}

/* ==================================================================
 * 具体构造：校验配置 -> 分配 -> 存注入指针 -> 推导几何
 *
 * 注意分工：本对象的装配不走抽象构造（BlockTiler 是本模块唯一具象，
 * 头文件只暴露不透明句柄，无抽象父类），但仍严格保留
 * 「校验 -> 分配 -> 初始化 -> 失败回滚 -> 返回」的分层写法；
 * 构造只保存注入指针与推导几何，绝不建线程、不碰硬件、不 post 任务。
 * ================================================================== */
BlockTiler* NewBlockTiler(const BlockTilerConfig* config)
{
    BlockTiler* self = NULL;
    int         ret  = BLOCK_TILER_OK;

    ASSERT_NOT_NULL(config, "blockTiler ctor: config is null", NULL);
    ASSERT_NOT_NULL(config->screen, "blockTiler ctor: config->screen is null", NULL);
    ASSERT_NOT_NULL(config->loop, "blockTiler ctor: config->loop is null", NULL);

    if (blockTilerMalloc == NULL) {
        return NULL;
    }

    self = (BlockTiler*)blockTilerMalloc(sizeof(BlockTiler));
    ASSERT_NOT_NULL(self, "blockTiler ctor: malloc failed", NULL);

    (void)memset(self, 0, sizeof(BlockTiler));
    self->screen = config->screen;
    self->loop   = config->loop;
    /* glyph 初始全 0：尚未 Show 时缓冲内容确定，不残留脏数据 */

    ret = blockTilerResolveGeometry(self);
    if (ret != BLOCK_TILER_OK) {
        /* 几何非法（尺寸不可用 / 非方形分块 / 单块边长超上限）：回滚并失败 */
        if (blockTilerFree != NULL) {
            blockTilerFree(self);
        }
        self = NULL;
        return NULL;
    }

    return self;
}

/* ==================================================================
 * 析构：只释放自身并置空外部指针
 *
 * 注入的 screen / loop 所有权不归本模块，禁止在此调用
 * DeleteScreen / DeleteEventLoop（否则会破坏调用方持有的对象）。
 * ================================================================== */
void DeleteBlockTiler(BlockTiler** pTiler)
{
    BlockTiler* self = NULL;

    if (pTiler == NULL) {
        return;
    }

    self = *pTiler;
    if (self == NULL) {
        return;
    }

    /* 先摘掉外部指针，再清空依赖与缓冲，最后释放，避免任何悬空访问窗口 */
    *pTiler = NULL;

    self->screen = NULL;
    self->loop   = NULL;

    if (blockTilerFree != NULL) {
        blockTilerFree(self);
    }
    self = NULL;
}

/* ==================================================================
 * 对外：平铺字母 T（异步）
 *
 * 调用线程只做两件事：把内置字形拷进对象内部缓冲、post 渲染任务，
 * 随后立即返回；绘制与 ScreenFlush 全在 loop 的 worker 线程内发生。
 * ================================================================== */
int BlockTilerShowLetterT(BlockTiler* tiler)
{
    ASSERT_NOT_NULL(tiler, "blockTiler showLetterT: tiler is null", BLOCK_TILER_ERR_PARAM);
    ASSERT_NOT_NULL(tiler->loop, "blockTiler showLetterT: loop is null", BLOCK_TILER_ERR_PARAM);
    ASSERT_NOT_NULL(tiler->screen, "blockTiler showLetterT: screen is null", BLOCK_TILER_ERR_PARAM);

    /* 先写缓冲、后入队：worker 出队时一定能看见完整字形 */
    (void)memcpy(tiler->glyph, kBlockTilerLetterT, sizeof(tiler->glyph));

    return blockTilerPostRender(tiler);
}

/* ==================================================================
 * 对外：平铺调用方给的 1 bit 位图（异步）
 *
 * 位图内容在 post 之前拷贝进对象内部缓冲，函数返回后调用方即可
 * 释放自己的 Bitmap（本模块不保存 glyph 指针）。
 * ================================================================== */
int BlockTilerShowGlyph(BlockTiler* tiler, const Bitmap* glyph)
{
    uint8_t local[BLOCK_TILER_GLYPH_ROWS];
    int     ret = BLOCK_TILER_OK;

    ASSERT_NOT_NULL(tiler, "blockTiler showGlyph: tiler is null", BLOCK_TILER_ERR_PARAM);
    ASSERT_NOT_NULL(glyph, "blockTiler showGlyph: glyph is null", BLOCK_TILER_ERR_PARAM);
    ASSERT_NOT_NULL(tiler->loop, "blockTiler showGlyph: loop is null", BLOCK_TILER_ERR_PARAM);
    ASSERT_NOT_NULL(tiler->screen, "blockTiler showGlyph: screen is null", BLOCK_TILER_ERR_PARAM);

    /* 先在局部缓冲里拼好 8 行，再一次性发布到对象内部，
     * 使 worker 看到的多字节更新只有单一发布点（不出现半更新的中间态） */
    ret = blockTilerPackGlyph(glyph, local);
    if (ret != BLOCK_TILER_OK) {
        return ret;
    }

    (void)memcpy(tiler->glyph, local, sizeof(tiler->glyph));

    return blockTilerPostRender(tiler);
}
