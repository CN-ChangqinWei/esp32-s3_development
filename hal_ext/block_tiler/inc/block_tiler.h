#ifndef _BLOCK_TILER_H
#define _BLOCK_TILER_H

/* ==================================================================
 * block_tiler.h —— 8x8 位图平铺自检服务（依赖注入式具象服务）
 *
 * 职责（只做一件事）：
 *   把「同一个字形 / 位图」平铺到屏幕的【每一个分块】的左上角，
 *   用于肉眼判断点阵模组的方向（上下是否翻转）、极性（亮灭是否取反）
 *   以及接线顺序（分块序列是否与坐标轴一致）。
 *
 * 分块概念：
 *   屏幕被等分为 chipsX 列 x chipsY 行个方块，每块 blockW x blockH 像素
 *   （本板为 8x8，即每个 MAX7219 单片）。第 (cx, cy) 块的左上角像素为
 *   (cx * blockW, cy * blockH)。几何全部由 ScreenWidth / ScreenHeight
 *   与构造参数推导，本模块不硬编码任何屏幕尺寸。
 *
 * 坐标约定（与 ScreenSetPixel 完全一致）：
 *   - 屏幕坐标原点在【左上角】，x 向右递增，y 向下递增；
 *   - 字形 / 位图的 row = 0 对应块的最上面一行，
 *     col = 0 对应块的最左列（即左上角对齐，不居中、不镜像）；
 *   - 位图中 bit 为 1 的像素调用 ScreenSetPixel(originX + col,
 *     originY + row, 1) 点亮；为 0 的像素【不动】，保持清屏后的背景。
 *
 * 线程边界（本模块最重要的约定）：
 *   调用线程                              event_loop worker 线程
 *   ------------------------------------  ----------------------------------
 *   BlockTilerShowLetterT                 blockTilerRenderTask（static）
 *     - 把字形拷进对象内部缓冲              - ScreenClear
 *     - post 渲染任务                       - 逐块逐像素 ScreenSetPixel
 *     - 立即返回                            - ScreenFlush
 *
 *   即：帧缓冲的【写】与下发硬件的【flush】只在注入的 EventLoop 的
 *   worker 线程内发生；调用线程从不直接调用 Screen 的任何槽位。
 *   字形缓冲的「先写、后 post」由 EventLoop 队列内部的互斥量提供
 *   释放-获取语义，因此 worker 取到任务时一定看得见本次写入的内容。
 *
 * 依赖方向（严格单向，只依赖抽象）：
 *   screen.h      Screen 抽象父类（像素写入 + flush）
 *   event_loop.h  EventLoop 抽象父类（跨线程任务队列）
 *   bitmap.h      1 bit 位图纯数据模块（仅作为入参载体，本模块不拥有它）
 *   不包含 freertos / driver / esp_*.h 等任何平台头。
 *
 * 所有权约定：
 *   config 里的 screen / loop 由调用方创建并拥有，本模块【只借用】：
 *   构造不创建线程、不碰硬件，析构也不删除这两个注入对象。
 *   BlockTilerShowGlyph 的 glyph 同样只借用，函数返回后调用方即可释放。
 *
 * 返回值约定（与 hal_ext 各模块口径一致）：
 *     0   成功（Show 系列 = 已入队）
 *    -1   参数非法（对象为空、注入依赖为空、位图为空 / 尺寸越界）
 *    -2   post 槽位未实现
 *   其余负值 = event_loop post 的返回值原样上抛（如队列已满）
 * ================================================================== */

#include <stdint.h>

/* screen.h 已带 <stddef.h>（size_t 来源）并已包含 bitmap.h，
 * 这里的 bitmap.h 仅为显式自证依赖；两个头都有 include guard，重复包含无代价。 */
#include "screen.h"
#include "event_loop.h"
#include "bitmap.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------
 * 本模块内存钩子：默认指向 malloc / free，
 * 外部可在创建对象之前替换为平台专用分配器（依赖反转）：
 *     blockTilerMalloc = pvPortMalloc;   blockTilerFree = vPortFree;
 * 仅用于本对象自身；注入的 Screen / EventLoop / Bitmap 内存不归本模块管理。
 * ------------------------------------------------------------------ */
extern void* (*blockTilerMalloc)(size_t size);
extern void  (*blockTilerFree)(void* p);

/* ------------------------------------------------------------------
 * 返回值约定宏
 * ------------------------------------------------------------------ */
#define BLOCK_TILER_OK           0
#define BLOCK_TILER_ERR_PARAM  (-1)
#define BLOCK_TILER_ERR_QUEUE  (-2)   /* 未实现：post 槽位为空 */

/* ------------------------------------------------------------------
 * 具象对象：字段不对外暴露（不完整类型），
 * 使用方一律以 BlockTiler* 句柄操作。
 * ------------------------------------------------------------------ */
typedef struct BlockTiler BlockTiler;

/* ------------------------------------------------------------------
 * 构造配置：外部依赖注入
 *
 *   screen : 显示抽象，本模块只调用其虚表（clear / setPixel / flush /
 *            getWidth / getHeight），不销毁它；
 *            严禁在此传入任何硬编码的屏幕具象，一律由调用方组装后传入。
 *   loop   : 渲染事件队列，其 worker 线程即帧缓冲的写入线程与 SPI
 *            下发线程；本模块不启动、不停止、不销毁该循环，只 post 任务。
 *   两者任一为空则构造失败。
 * ------------------------------------------------------------------ */
typedef struct BlockTilerConfig {
    Screen*    screen;   /* 注入：显示抽象，不得硬编码任何具体屏幕对象 */
    EventLoop* loop;     /* 注入：绘制 / 刷屏必须发生在其 worker 线程 */
} BlockTilerConfig;

/* ------------------------------------------------------------------
 * 构造 / 析构
 *
 * NewBlockTiler：
 *   1) 校验 config 与其两个注入指针非空；
 *   2) 用 ScreenWidth / ScreenHeight 推导 blockW / blockH / chipsX / chipsY；
 *   3) 只保存注入指针与推导出的几何，不建线程、不碰硬件、不 post。
 *   几何合法性要求：blockW == blockH、单块边长不超过字形边长（8）、
 *   chipsX 与 chipsY 均不小于 1；不满足则返回 NULL。
 *   失败（config 为空、注入指针为空、屏幕尺寸不可用、几何非法、内存不足）
 *   一律返回 NULL。
 *
 * DeleteBlockTiler：
 *   只释放本对象自身并把外部指针置空；注入的 screen / loop 不在此销毁
 *   （禁止调用 DeleteScreen / DeleteEventLoop）。
 *   允许传入 NULL 或指向 NULL 的指针（空操作）。
 * ------------------------------------------------------------------ */
BlockTiler* NewBlockTiler(const BlockTilerConfig* config);
void        DeleteBlockTiler(BlockTiler** pTiler);

/* ------------------------------------------------------------------
 * 在每个分块的【左上角】画同一个字母 T（异步）
 *
 * 字形：8 行 x 8 列，行优先，最高位对应最左列；
 *   首行整行点亮（顶横），其下 7 行只点亮居中两列（竖笔）。
 *   上下 / 左右若被翻转一眼可辨，故适合做方向与极性自检。
 *
 * 动作：把内置字形拷进对象内部缓冲 -> post 渲染任务 -> 立即返回。
 * 返回：BLOCK_TILER_OK 成功（已入队）；
 *       BLOCK_TILER_ERR_PARAM 对象 / 注入依赖为空；
 *       BLOCK_TILER_ERR_QUEUE post 槽位为空；
 *       其余负值 = post 的返回值原样上抛。
 * ------------------------------------------------------------------ */
int BlockTilerShowLetterT(BlockTiler* tiler);

/* ------------------------------------------------------------------
 * 通用版：把调用方给的 1 bit 位图平铺到每一个分块的左上角（异步）
 *
 * 入参校验：glyph 非空、glyph->at 非空、宽高均大于 0 且不超过 8。
 *
 * 拷贝语义：位图内容在 post 之前【拷贝】进对象内部缓冲，
 *   本函数返回后调用方即可释放自己的 Bitmap（本模块不持有其指针）。
 *
 * 归一化规则：位图第 col 列固定落在块的 bit 位 (7 - col) 上，
 *   与位图实际宽高无关；位图窄于 8 列时靠左对齐，低位补 0；
 *   位图矮于 8 行时下方补 0。
 *
 * 返回：BLOCK_TILER_OK 成功（已入队）；
 *       BLOCK_TILER_ERR_PARAM 参数非法（空指针 / 宽高越界）；
 *       BLOCK_TILER_ERR_QUEUE post 槽位为空；
 *       其余负值 = post 的返回值原样上抛。
 * ------------------------------------------------------------------ */
int BlockTilerShowGlyph(BlockTiler* tiler, const Bitmap* glyph);

#ifdef __cplusplus
}
#endif

#endif /* _BLOCK_TILER_H */
