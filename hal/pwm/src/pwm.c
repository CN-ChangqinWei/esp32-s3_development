/**
 * @file pwm.c
 * @brief PWM 抽象父类实现：内存钩子默认值 + 抽象构造/析构 + 包装函数透传。
 *
 * 本文件不含任何平台相关代码（无 ESP-IDF / FreeRTOS 依赖），
 * 只做「分配 → 存 instance → 存 interfaces」以及「判空 → 透传」。
 * 通道号越界、分辨率上限、千分比 -> 占空计数的换算均属于具象层职责。
 */

#include "pwm.h"

#include <stdlib.h>

/* ------------------------------------------------------------------
 * 返回值约定（与 pwm.h 保持一致）
 * ------------------------------------------------------------------ */
#define PWM_OK              0
#define PWM_ERR_PARAM     (-1)
#define PWM_ERR_UNSUPPORTED (-2)

/* 占空千分比上限：1000‰ = 100%（抽象层只做范围校验，不做换算） */
#define PWM_PERMILLE_MAX    1000u

/* ------------------------------------------------------------------
 * 内存钩子默认实现：外部可在创建对象之前替换为平台专用分配器
 *     pwmMalloc = pvPortMalloc;   pwmFree = vPortFree;
 * ------------------------------------------------------------------ */
void* (*pwmMalloc)(size_t size) = malloc;
void  (*pwmFree)(void* p)       = free;

/* ------------------------------------------------------------------
 * 内部工具：抽象对象与具体实例都必须存在
 * ------------------------------------------------------------------ */
static int pwmCheckHandle(const Pwm* pwm) {
    if (pwm == NULL) {
        return PWM_ERR_PARAM;
    }
    if (pwm->instance == NULL) {
        return PWM_ERR_PARAM;
    }
    return PWM_OK;
}

/* ------------------------------------------------------------------
 * 抽象构造：只做三件事 —— 分配抽象对象 / 赋 instance / 赋 interfaces
 * 不 malloc 具体类型、不解析任何业务参数。
 * ------------------------------------------------------------------ */
Pwm* NewPwm(void* instance, PwmInterface interfaces) {
    Pwm* self = NULL;

    /* 具体实例与析构函数必须存在，否则 DeletePwm 将泄漏具体实例 */
    if (instance == NULL || interfaces.deleteInstance == NULL) {
        return NULL;
    }
    if (pwmMalloc == NULL) {
        return NULL;
    }

    self = (Pwm*)pwmMalloc(sizeof(Pwm));
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
void DeletePwm(Pwm** pPwm) {
    Pwm* self = NULL;

    if (pPwm == NULL) {
        return;
    }

    self = *pPwm;
    if (self == NULL) {
        return;
    }

    /* ① 销毁具体实例 */
    if (self->interfaces.deleteInstance != NULL) {
        self->interfaces.deleteInstance(self->instance);
    }

    self->instance                  = NULL;
    self->interfaces.deleteInstance = NULL;

    /* ② 释放抽象对象本身 */
    if (pwmFree != NULL) {
        pwmFree(self);
    }
    self = NULL;

    /* ③ 外部指针置空，防止悬空 */
    *pPwm = NULL;
}

/* ------------------------------------------------------------------
 * 包装函数：先判空 / 判非法入参，再透传虚表
 *   返回  0 ：虚表调用成功返回
 *   返回 -1 ：pwm 为空、instance 为空、config 为空、频率/分辨率非法、
 *              千分比越界（通道号越界由具象层判定）
 *   返回 -2 ：对应接口未实现
 * ------------------------------------------------------------------ */
int PwmInit(Pwm* pwm) {
    if (pwmCheckHandle(pwm) != PWM_OK) {
        return PWM_ERR_PARAM;
    }
    if (pwm->interfaces.init == NULL) {
        return PWM_ERR_UNSUPPORTED;
    }

    return pwm->interfaces.init(pwm->instance);
}

int PwmDeinit(Pwm* pwm) {
    if (pwmCheckHandle(pwm) != PWM_OK) {
        return PWM_ERR_PARAM;
    }
    if (pwm->interfaces.deinit == NULL) {
        return PWM_ERR_UNSUPPORTED;
    }

    return pwm->interfaces.deinit(pwm->instance);
}

int PwmConfigure(Pwm* pwm, uint32_t channel, const PwmConfig* config) {
    if (pwmCheckHandle(pwm) != PWM_OK || config == NULL) {
        return PWM_ERR_PARAM;
    }
    if (config->frequencyHz == 0u || config->resolutionBits == 0u) {
        return PWM_ERR_PARAM;
    }
    if (pwm->interfaces.configure == NULL) {
        return PWM_ERR_UNSUPPORTED;
    }

    return pwm->interfaces.configure(pwm->instance, channel, config);
}

int PwmSetFrequency(Pwm* pwm, uint32_t channel, uint32_t frequencyHz) {
    if (pwmCheckHandle(pwm) != PWM_OK || frequencyHz == 0u) {
        return PWM_ERR_PARAM;
    }
    if (pwm->interfaces.setFrequency == NULL) {
        return PWM_ERR_UNSUPPORTED;
    }

    return pwm->interfaces.setFrequency(pwm->instance, channel, frequencyHz);
}

int PwmSetDuty(Pwm* pwm, uint32_t channel, uint32_t duty) {
    if (pwmCheckHandle(pwm) != PWM_OK) {
        return PWM_ERR_PARAM;
    }
    if (pwm->interfaces.setDuty == NULL) {
        return PWM_ERR_UNSUPPORTED;
    }

    /* duty 的合法上限 = 2^resolutionBits - 1，取决于具象层已配置的分辨率 */
    return pwm->interfaces.setDuty(pwm->instance, channel, duty);
}

int PwmSetDutyPermille(Pwm* pwm, uint32_t channel, uint32_t permille) {
    if (pwmCheckHandle(pwm) != PWM_OK || permille > PWM_PERMILLE_MAX) {
        return PWM_ERR_PARAM;
    }
    if (pwm->interfaces.setDutyPermille == NULL) {
        return PWM_ERR_UNSUPPORTED;
    }

    /* 千分比 -> 占空计数的换算依赖分辨率，必须由具象层完成 */
    return pwm->interfaces.setDutyPermille(pwm->instance, channel, permille);
}

int PwmStart(Pwm* pwm, uint32_t channel) {
    if (pwmCheckHandle(pwm) != PWM_OK) {
        return PWM_ERR_PARAM;
    }
    if (pwm->interfaces.start == NULL) {
        return PWM_ERR_UNSUPPORTED;
    }

    return pwm->interfaces.start(pwm->instance, channel);
}

int PwmStop(Pwm* pwm, uint32_t channel) {
    if (pwmCheckHandle(pwm) != PWM_OK) {
        return PWM_ERR_PARAM;
    }
    if (pwm->interfaces.stop == NULL) {
        return PWM_ERR_UNSUPPORTED;
    }

    return pwm->interfaces.stop(pwm->instance, channel);
}
