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
