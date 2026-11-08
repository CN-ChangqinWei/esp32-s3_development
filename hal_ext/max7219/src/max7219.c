/**
 * @file max7219.c
 * @brief MAX7219 抽象父类实现：内存钩子默认值 + 抽象构造/析构 + 包装函数透传。
 *
 * 本文件不含任何平台相关代码（无 FreeRTOS / ESP-IDF / GPIO 依赖），
 * 只做「分配 -> 存 instance -> 存 interfaces」以及「判空 -> 判范围 -> 透传」。
 * 片选、时序、寄存器编解码、链长校验均由具象层负责。
 *
 * 返回值约定（与 max7219.h 保持一致）：
 *    0  成功
 *   -1  参数非法（driver 空、instance 空、intensity 大于 15、digits 不在 1 到 8、
 *                  frame 空、len 为 0 或为奇数；chipIndex 越界由具象层判定）
 *   -2  虚表槽位为空（该能力未实现）
 */

#include "max7219.h"

#include <stdlib.h>

/* ------------------------------------------------------------------
 * 返回值约定
 * ------------------------------------------------------------------ */
#define MAX7219_OK              0
#define MAX7219_ERR_PARAM       (-1)
#define MAX7219_ERR_UNSUPPORTED (-2)

/* 参数合法区间（抽象层只判范围，语义归具象层）；
 * intensity 为 uint8_t，下界恒为 0，故只判上界，避免恒假比较告警 */
#define MAX7219_INTENSITY_MAX  15u
#define MAX7219_SCAN_LIMIT_MIN 1u
#define MAX7219_SCAN_LIMIT_MAX 8u

/* ------------------------------------------------------------------
 * 内存钩子默认实现：外部可在创建对象之前替换为平台专用分配器
 *     max7219Malloc = pvPortMalloc;   max7219Free = vPortFree;
 * ------------------------------------------------------------------ */
void* (*max7219Malloc)(size_t size) = malloc;
void  (*max7219Free)(void* p)       = free;

/* ------------------------------------------------------------------
 * 内部工具：抽象对象与具体实例都必须存在
 * ------------------------------------------------------------------ */
static int max7219CheckHandle(const Max7219* driver) {
    if (driver == NULL) {
        return MAX7219_ERR_PARAM;
    }
    if (driver->instance == NULL) {
        return MAX7219_ERR_PARAM;
    }
    return MAX7219_OK;
}

/* ------------------------------------------------------------------
 * 抽象构造：只做三件事 —— 分配抽象对象 / 赋 instance / 赋 interfaces
 * 不 malloc 具体类型、不解析任何业务参数。
 * ------------------------------------------------------------------ */
Max7219* NewMax7219(void* instance, Max7219Interface interfaces) {
    Max7219* self = NULL;

    /* 具体实例与析构函数必须存在，否则 DeleteMax7219 将泄漏具体实例 */
    if (instance == NULL || interfaces.deleteInstance == NULL) {
        return NULL;
    }
    if (max7219Malloc == NULL) {
        return NULL;
    }

    self = (Max7219*)max7219Malloc(sizeof(Max7219));
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
void DeleteMax7219(Max7219** pMax7219) {
    Max7219* self = NULL;

    if (pMax7219 == NULL) {
        return;
    }

    self = *pMax7219;
    if (self == NULL) {
        return;
    }

    /* ① 销毁具体实例 */
    if (self->interfaces.deleteInstance != NULL && self->instance != NULL) {
        self->interfaces.deleteInstance(self->instance);
    }

    self->instance                  = NULL;
    self->interfaces.deleteInstance = NULL;

    /* ② 释放抽象对象本身 */
    if (max7219Free != NULL) {
        max7219Free(self);
    }
    self = NULL;

    /* ③ 外部指针置空，防止悬空 */
    *pMax7219 = NULL;
}

/* ------------------------------------------------------------------
 * 包装函数：先判空 / 判非法入参，再透传虚表
 * ------------------------------------------------------------------ */

/* 上电初始化：上电默认配置由具象层决定，抽象层只透传 */
int Max7219Init(Max7219* driver) {
    if (max7219CheckHandle(driver) != MAX7219_OK) {
        return MAX7219_ERR_PARAM;
    }
    if (driver->interfaces.init == NULL) {
        return MAX7219_ERR_UNSUPPORTED;
    }

    return driver->interfaces.init(driver->instance);
}

/* enable 非 0 -> 进入 shutdown 模式（关闭显示）；为 0 -> 恢复正常显示 */
int Max7219Shutdown(Max7219* driver, int enable) {
    if (max7219CheckHandle(driver) != MAX7219_OK) {
        return MAX7219_ERR_PARAM;
    }
    if (driver->interfaces.shutdown == NULL) {
        return MAX7219_ERR_UNSUPPORTED;
    }

    return driver->interfaces.shutdown(driver->instance, (0 != enable) ? 1 : 0);
}

/* 亮度：0 到 15 */
int Max7219SetIntensity(Max7219* driver, uint8_t intensity) {
    if (max7219CheckHandle(driver) != MAX7219_OK) {
        return MAX7219_ERR_PARAM;
    }
    if (intensity > MAX7219_INTENSITY_MAX) {
        return MAX7219_ERR_PARAM;
    }
    if (driver->interfaces.setIntensity == NULL) {
        return MAX7219_ERR_UNSUPPORTED;
    }

    return driver->interfaces.setIntensity(driver->instance, intensity);
}

/* 有效扫描位数：1 到 8 */
int Max7219SetScanLimit(Max7219* driver, uint8_t digits) {
    if (max7219CheckHandle(driver) != MAX7219_OK) {
        return MAX7219_ERR_PARAM;
    }
    if (digits < MAX7219_SCAN_LIMIT_MIN || digits > MAX7219_SCAN_LIMIT_MAX) {
        return MAX7219_ERR_PARAM;
    }
    if (driver->interfaces.setScanLimit == NULL) {
        return MAX7219_ERR_UNSUPPORTED;
    }

    return driver->interfaces.setScanLimit(driver->instance, digits);
}

/* 译码模式：0x00 不译码、0xFF BCD 全译码；8 位位图任意取值均合法，不做范围校验 */
int Max7219SetDecodeMode(Max7219* driver, uint8_t mode) {
    if (max7219CheckHandle(driver) != MAX7219_OK) {
        return MAX7219_ERR_PARAM;
    }
    if (driver->interfaces.setDecodeMode == NULL) {
        return MAX7219_ERR_UNSUPPORTED;
    }

    return driver->interfaces.setDecodeMode(driver->instance, mode);
}

/* 清空全部显示寄存器（链上所有片，具体广播方式归具象层）*/
int Max7219Clear(Max7219* driver) {
    if (max7219CheckHandle(driver) != MAX7219_OK) {
        return MAX7219_ERR_PARAM;
    }
    if (driver->interfaces.clear == NULL) {
        return MAX7219_ERR_UNSUPPORTED;
    }

    return driver->interfaces.clear(driver->instance);
}

/* 单寄存器写：chipIndex = 0 为直接接 DIN 的第一片；越界判定归具象层 */
int Max7219WriteRegister(Max7219* driver, uint32_t chipIndex, uint8_t reg, uint8_t value) {
    if (max7219CheckHandle(driver) != MAX7219_OK) {
        return MAX7219_ERR_PARAM;
    }
    if (driver->interfaces.writeRegister == NULL) {
        return MAX7219_ERR_UNSUPPORTED;
    }

    return driver->interfaces.writeRegister(driver->instance, chipIndex, reg, value);
}

/* 整帧写：frame 按「先发最远端」排列，len 必须为偶数（= 2 * chainLength）。
 * 链长一致性校验由具象层完成，抽象层只保证长度非 0 且为偶数。 */
int Max7219WriteFrame(Max7219* driver, const uint8_t* frame, uint32_t len) {
    if (max7219CheckHandle(driver) != MAX7219_OK) {
        return MAX7219_ERR_PARAM;
    }
    if (frame == NULL) {
        return MAX7219_ERR_PARAM;
    }
    if (0u == len || 0u != (len % 2u)) {
        return MAX7219_ERR_PARAM;
    }
    if (driver->interfaces.writeFrame == NULL) {
        return MAX7219_ERR_UNSUPPORTED;
    }

    return driver->interfaces.writeFrame(driver->instance, frame, len);
}
