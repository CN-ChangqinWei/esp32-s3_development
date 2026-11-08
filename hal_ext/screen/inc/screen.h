#ifndef _SCREEN_H
#define _SCREEN_H

/* ==================================================================
 * screen.h —— Screen 抽象父类（OOP in C）
 *
 * 结构：
 *   抽象对象 Screen = { void* instance; ScreenInterface interfaces; }
 *   具体实现（max7219 点阵、OLED、虚拟缓冲等）负责 instance 的真实类型
 *   与全部接口函数；抽象层只负责「装配 + 判空 + 透传」，
 *   不含宽高、引脚、驱动指针等任何具体字段。
 *
 * 依赖：
 *   只允许 <stdint.h> / <stddef.h>，纯抽象、零平台依赖，
 *   禁止包含 freertos、driver 下的平台头与 esp_*.h 等平台头。
 *
 * 返回值约定：
 *    0   成功
 *   -1   参数非法（screen 为空、instance 为空；getPixel 坐标越界）
 *   -2   未实现 / 不支持（对应接口槽位为空）
 *   其余负值为具体实现自定义错误码
 *
 * 特例：
 *   getPixel 成功时返回 1 表示亮、0 表示灭；坐标非法返回 -1。
 *   getWidth / getHeight 成功时返回正整数；槽位为空返回 -2。
 *
 * 像素坐标约定（由具体实现遵守，抽象层不校验）：
 *   左上角为原点，x 向右递增，y 向下递增，
 *   合法范围 x 属于 [0, width)，y 属于 [0, height)。
 * ================================================================== */

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------
 * 内存钩子：默认指向 malloc / free（定义在 screen.c），
 * 外部可在创建对象之前替换为平台专用分配器：
 *     screenMalloc = pvPortMalloc;  screenFree = vPortFree;
 * ------------------------------------------------------------------ */
extern void* (*screenMalloc)(size_t size);
extern void  (*screenFree)(void* p);

/* ------------------------------------------------------------------
 * 返回值约定宏
 * ------------------------------------------------------------------ */
#define SCREEN_OK                 0
#define SCREEN_ERR_PARAM        (-1)
#define SCREEN_ERR_UNSUPPORTED  (-2)

/* ------------------------------------------------------------------
 * 虚表：由具体实现填充；抽象层只调用，不实现
 *
 * 亮度语义统一为 int：
 *   on 非 0 表示点亮，on 为 0 表示熄灭。
 * 坐标类型统一为 int32_t，避免大尺寸屏溢出。
 * ------------------------------------------------------------------ */
typedef struct ScreenInterface {
    int (*init)(void* instance);
    int (*deinit)(void* instance);
    int (*clear)(void* instance);                              /* 全屏熄灭 */
    int (*fill)(void* instance, int on);                       /* 全屏点亮/熄灭 */
    int (*setPixel)(void* instance, int32_t x, int32_t y, int on);
    int (*getPixel)(void* instance, int32_t x, int32_t y);     /* 返回 1/0；坐标非法返回 -1 */
    int (*flush)(void* instance);                              /* 把内存帧推到硬件 */
    int (*getWidth)(void* instance);                           /* 返回像素宽，负值为错误 */
    int (*getHeight)(void* instance);                          /* 返回像素高，负值为错误 */
    void (*deleteInstance)(void* instance);
} ScreenInterface;

/* ------------------------------------------------------------------
 * 抽象对象：只允许 instance + interfaces 两个字段
 * ------------------------------------------------------------------ */
typedef struct Screen {
    void*           instance;
    ScreenInterface interfaces;
} Screen;

/* ==================================================================
 * _RETURN_SCREEN_INTERFACE
 * 供具体实现集中返回接口表，约定实现侧存在以下同名 static 函数：
 *
 *   static int  init(void* instance);
 *   static int  deinit(void* instance);
 *   static int  clear(void* instance);
 *   static int  fill(void* instance, int on);
 *   static int  setPixel(void* instance, int32_t x, int32_t y, int on);
 *   static int  getPixel(void* instance, int32_t x, int32_t y);
 *   static int  flush(void* instance);
 *   static int  getWidth(void* instance);
 *   static int  getHeight(void* instance);
 *   static void deleteInstance(void* instance);
 *
 * 用法（具体实现文件里）：
 *   ScreenInterface ScreenXxxInterface(void) {
 *       _RETURN_SCREEN_INTERFACE;
 *   }
 *
 * 备注：若函数名与已有符号冲突，可改用带参版本
 *       _RETURN_SCREEN_INTERFACE_WITH(...) 传入自己的函数名。
 *       两个宏均以 return interfaces 结尾且不带分号，调用处按语句加分号。
 * ================================================================== */
#define _RETURN_SCREEN_INTERFACE_WITH(initFunc, deinitFunc, clearFunc, fillFunc,      \
                                      setPixelFunc, getPixelFunc, flushFunc,          \
                                      getWidthFunc, getHeightFunc, deleteFunc)        \
    ScreenInterface interfaces = {                \
        .init           = (initFunc),             \
        .deinit         = (deinitFunc),           \
        .clear          = (clearFunc),            \
        .fill           = (fillFunc),             \
        .setPixel       = (setPixelFunc),         \
        .getPixel       = (getPixelFunc),         \
        .flush          = (flushFunc),            \
        .getWidth       = (getWidthFunc),         \
        .getHeight      = (getHeightFunc),        \
        .deleteInstance = (deleteFunc),           \
    };                                            \
    return interfaces

#define _RETURN_SCREEN_INTERFACE                                                        \
    _RETURN_SCREEN_INTERFACE_WITH(init, deinit, clear, fill, setPixel, getPixel,        \
                                  flush, getWidth, getHeight, deleteInstance)

/* ------------------------------------------------------------------
 * 抽象构造 / 抽象析构
 * ------------------------------------------------------------------ */
Screen* NewScreen(void* instance, ScreenInterface interfaces);
void    DeleteScreen(Screen** pScreen);

/* ------------------------------------------------------------------
 * 包装函数：判空 + 透传虚表
 * ------------------------------------------------------------------ */
int ScreenInit(Screen* screen);
int ScreenDeinit(Screen* screen);
int ScreenClear(Screen* screen);
int ScreenFill(Screen* screen, int on);
int ScreenSetPixel(Screen* screen, int32_t x, int32_t y, int on);
int ScreenGetPixel(Screen* screen, int32_t x, int32_t y);
int ScreenFlush(Screen* screen);
int ScreenWidth(Screen* screen);
int ScreenHeight(Screen* screen);

#ifdef __cplusplus
}
#endif

#endif /* _SCREEN_H */
