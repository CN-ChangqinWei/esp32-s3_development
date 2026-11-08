/**
 * @file screen.c
 * @brief Screen 抽象父类实现：内存钩子默认值 + 抽象构造/析构 + 包装函数透传。
 *
 * 本文件不含任何平台相关代码（无 ESP-IDF / FreeRTOS 依赖），
 * 只做「分配 → 存 instance → 存 interfaces」以及「判空 → 透传」。
 */

#include "screen.h"

#include <stdlib.h>

/* ------------------------------------------------------------------
 * 内存钩子默认实现：外部可在创建对象之前替换为平台专用分配器
 *     screenMalloc = pvPortMalloc;   screenFree = vPortFree;
 * ------------------------------------------------------------------ */
void* (*screenMalloc)(size_t size) = malloc;
void  (*screenFree)(void* p)       = free;

/* ------------------------------------------------------------------
 * 内部工具
 * ------------------------------------------------------------------ */

/* 抽象对象与具体实例都必须存在；0 表示可用，-1 表示参数非法 */
static int screenCheckHandle(const Screen* screen) {
    if (screen == NULL) {
        return SCREEN_ERR_PARAM;
    }
    if (screen->instance == NULL) {
        return SCREEN_ERR_PARAM;
    }
    return SCREEN_OK;
}

/* 亮度入参归一化：非 0 一律视为点亮 */
static int screenNormalizeOn(int on) {
    return (on != 0) ? 1 : 0;
}

/* ------------------------------------------------------------------
 * 抽象构造：只做三件事 —— 分配抽象对象 / 赋 instance / 赋 interfaces
 * 不 malloc 具体类型、不解析任何业务参数（宽高由具象经 getWidth/getHeight 暴露）
 * ------------------------------------------------------------------ */
Screen* NewScreen(void* instance, ScreenInterface interfaces) {
    Screen* self = NULL;

    if (NULL == instance || NULL == interfaces.deleteInstance) {
        return NULL;
    }
    if (NULL == screenMalloc) {
        return NULL;
    }

    self = (Screen*)screenMalloc(sizeof(Screen));
    if (self == NULL) {
        return NULL;
    }

    self->instance   = instance;
    self->interfaces = interfaces;

    return self;
}

/* ------------------------------------------------------------------
 * 抽象析构：先销毁具体实例，再释放抽象对象，最后置空入参指针
 * ------------------------------------------------------------------ */
void DeleteScreen(Screen** pScreen) {
    Screen* self = NULL;

    if (pScreen == NULL) {
        return;
    }

    self = *pScreen;
    if (self == NULL) {
        return;
    }

    if (self->interfaces.deleteInstance != NULL) {
        self->interfaces.deleteInstance(self->instance);
    }

    self->instance                  = NULL;
    self->interfaces.deleteInstance = NULL;

    if (screenFree != NULL) {
        screenFree(self);
    }
    self = NULL;

    *pScreen = NULL;
}

/* ------------------------------------------------------------------
 * 包装函数：先判空 / 判非法入参，再透传虚表
 *   返回 0  ：虚表调用成功返回
 *   返回 -1 ：screen 为空、instance 为空
 *   返回 -2 ：对应接口未实现
 * ------------------------------------------------------------------ */
int ScreenInit(Screen* screen) {
    int rc = screenCheckHandle(screen);

    if (rc != SCREEN_OK) {
        return rc;
    }
    if (screen->interfaces.init == NULL) {
        return SCREEN_ERR_UNSUPPORTED;
    }

    return screen->interfaces.init(screen->instance);
}

int ScreenDeinit(Screen* screen) {
    int rc = screenCheckHandle(screen);

    if (rc != SCREEN_OK) {
        return rc;
    }
    if (screen->interfaces.deinit == NULL) {
        return SCREEN_ERR_UNSUPPORTED;
    }

    return screen->interfaces.deinit(screen->instance);
}

int ScreenClear(Screen* screen) {
    int rc = screenCheckHandle(screen);

    if (rc != SCREEN_OK) {
        return rc;
    }
    if (screen->interfaces.clear == NULL) {
        return SCREEN_ERR_UNSUPPORTED;
    }

    return screen->interfaces.clear(screen->instance);
}

int ScreenFill(Screen* screen, int on) {
    int rc = screenCheckHandle(screen);

    if (rc != SCREEN_OK) {
        return rc;
    }
    if (screen->interfaces.fill == NULL) {
        return SCREEN_ERR_UNSUPPORTED;
    }

    return screen->interfaces.fill(screen->instance, screenNormalizeOn(on));
}

int ScreenSetPixel(Screen* screen, int32_t x, int32_t y, int on) {
    int rc = screenCheckHandle(screen);

    if (rc != SCREEN_OK) {
        return rc;
    }
    if (screen->interfaces.setPixel == NULL) {
        return SCREEN_ERR_UNSUPPORTED;
    }

    return screen->interfaces.setPixel(screen->instance, x, y, screenNormalizeOn(on));
}

/* 坐标合法性由具体实现判断：越界返回 -1，成功返回 1（亮）或 0（灭） */
int ScreenGetPixel(Screen* screen, int32_t x, int32_t y) {
    int rc = screenCheckHandle(screen);

    if (rc != SCREEN_OK) {
        return rc;
    }
    if (screen->interfaces.getPixel == NULL) {
        return SCREEN_ERR_UNSUPPORTED;
    }

    return screen->interfaces.getPixel(screen->instance, x, y);
}

int ScreenFlush(Screen* screen) {
    int rc = screenCheckHandle(screen);

    if (rc != SCREEN_OK) {
        return rc;
    }
    if (screen->interfaces.flush == NULL) {
        return SCREEN_ERR_UNSUPPORTED;
    }

    return screen->interfaces.flush(screen->instance);
}

/* 成功返回像素宽（正整数）；槽位为空返回 -2；句柄非法返回 -1 */
int ScreenWidth(Screen* screen) {
    int rc = screenCheckHandle(screen);

    if (rc != SCREEN_OK) {
        return rc;
    }
    if (screen->interfaces.getWidth == NULL) {
        return SCREEN_ERR_UNSUPPORTED;
    }

    return screen->interfaces.getWidth(screen->instance);
}

/* 成功返回像素高（正整数）；槽位为空返回 -2；句柄非法返回 -1 */
int ScreenHeight(Screen* screen) {
    int rc = screenCheckHandle(screen);

    if (rc != SCREEN_OK) {
        return rc;
    }
    if (screen->interfaces.getHeight == NULL) {
        return SCREEN_ERR_UNSUPPORTED;
    }

    return screen->interfaces.getHeight(screen->instance);
}

/* ------------------------------------------------------------------
 * 复用层增强：位图绘制
 *
 * 设计要点：
 *   1. 不新增虚表槽位，只依赖既有 setPixel 组装，具象类零改动。
 *   2. 屏幕宽高各取一次并缓存，避免逐像素调用 ScreenWidth / ScreenHeight。
 *   3. 先做整行越界判断再逐像素判断目标列，越界像素不下传给具象实现。
 *   4. bit 为 1 才点亮，bit 为 0 视为透明、保持屏幕原内容。
 *   5. 坐标为 int32_t，x / y 可为负，靠裁剪而非报错处理。
 * ------------------------------------------------------------------ */
int ScreenDrawBitmap(Screen* screen, int32_t x, int32_t y, const Bitmap* bitmap) {
    int rc = screenCheckHandle(screen);
    int32_t widthBitmap = 0;
    int32_t heightBitmap = 0;
    int32_t widthScreen = 0;
    int32_t heightScreen = 0;
    int32_t row = 0;
    int32_t col = 0;

    if (rc != SCREEN_OK) {
        return rc;
    }
    if (bitmap == NULL || bitmap->at == NULL) {
        return SCREEN_ERR_PARAM;
    }
    if (bitmap->width <= 0 || bitmap->height <= 0) {
        return SCREEN_ERR_PARAM;
    }
    if (screen->interfaces.setPixel == NULL) {
        return SCREEN_ERR_UNSUPPORTED;
    }

    widthBitmap  = (int32_t)bitmap->width;
    heightBitmap = (int32_t)bitmap->height;
    widthScreen  = (int32_t)ScreenWidth(screen);
    heightScreen = (int32_t)ScreenHeight(screen);
    if (widthScreen <= 0 || heightScreen <= 0) {
        /* 屏幕尺寸不可用：无有效绘图区域，不做任何绘制 */
        return SCREEN_OK;
    }

    for (row = 0; row < heightBitmap; row++) {
        int32_t targetY = y + row;

        if (targetY < 0 || targetY >= heightScreen) {
            continue;   /* 整行落在屏幕外，无需逐像素判断 */
        }
        for (col = 0; col < widthBitmap; col++) {
            int32_t targetX = x + col;

            if (targetX < 0 || targetX >= widthScreen) {
                continue;   /* 该列落在屏幕外，裁剪掉 */
            }
            if (bitmap->at((Bitmap*)bitmap, (int)row, (int)col) == 0) {
                continue;   /* bit 为 0 视为透明 */
            }
            rc = ScreenSetPixel(screen, targetX, targetY, 1);
            if (rc < 0) {
                return rc;  /* 底层写入失败：原样上报，中止本次绘制 */
            }
        }
    }

    return SCREEN_OK;
}
