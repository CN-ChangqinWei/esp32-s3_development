#ifndef _PWM_H
#define _PWM_H

/* ==========================================================================
 * PWM 抽象层（父类，纯 C，零平台依赖）
 *
 * OOP in C 结构：
 *   ① PwmInterface = 虚函数表（V-Table），每个接口首参恒为 void* instance；
 *   ② Pwm          = 抽象类结构体，只放 void* instance + PwmInterface interfaces；
 *   ③ NewPwm       = 抽象类构造，只做「分配 + 存 instance + 存 interfaces」，
 *                    禁止在内部创建任何具体实例；
 *   ④ DeletePwm    = 抽象类析构，先调 deleteInstance 销毁具体实例，
 *                    再 pwmFree 抽象对象，最后把外部指针置 NULL；
 *   ⑤ 具象实现（如 hal/pwm/src/pwm_esp32c3.c 的 LEDC）在自己的 .c 中实现
 *      接口 static 函数，并提供具体构造 New<Xxx>(...)，其内部必须调用
 *      NewPwm 完成装配；接口表用本头文件的 _RETURN_PWM_INTERFACE 宏统一返回。
 *
 * 职责划分（重要）：
 *   - 通道号是否越界、resolutionBits 的平台上限、以及「千分比 -> 占空计数」
 *     的换算全部属于具象层（不同平台 LEDC 通道数 / 最大分辨率不同）；
 *   - 抽象层只做空指针与范围合法性校验，然后原样透传。
 *
 * 返回值约定（全部对外函数统一）：
 *    0 : 成功
 *   -1 : 参数非法（pwm 为空、instance 为空、config 为空、
 *                   frequencyHz == 0、resolutionBits == 0、permille > 1000）
 *   -2 : 未实现 / 不支持（该实现未提供对应的虚表槽位）
 *
 * 约束：本头文件不得包含 freertos、driver 下的头文件与 esp_*.h（只用 C 标准头），
 *       以保证使用方（main 等）只依赖抽象头，不被拖入平台头文件链。
 * ========================================================================== */

#include <stdint.h>
#include <stddef.h>

#if defined(__cplusplus)
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * 模块级内存管理接口：默认指向 malloc / free（见 pwm.c），
 * 具象模块或应用层可整体替换（如改用 pvPortMalloc / vPortFree）。
 * -------------------------------------------------------------------------- */
extern void* (*pwmMalloc)(size_t size);
extern void  (*pwmFree)(void* p);

/* --------------------------------------------------------------------------
 * PWM 配置值类型（纯数据，不含任何平台类型）
 *   frequencyHz    : 目标频率，必须 > 0
 *   resolutionBits : 分辨率位数，1..14（ESP32-C3 LEDC 上限 14；
 *                    上限校验由具象层完成，抽象层只拒绝 0）
 * -------------------------------------------------------------------------- */
typedef struct PwmConfig {
    uint32_t frequencyHz;      /* 目标频率 */
    uint8_t  resolutionBits;   /* 分辨率位数，1..14（ESP32-C3 LEDC 上限 14） */
} PwmConfig;

/* --------------------------------------------------------------------------
 * ① 接口表（虚函数表 V-Table）
 *    首参恒为 void* instance（具象实例），由具象层 static 函数实现。
 *    注意：析构槽位名必须是 deleteInstance（不可写成 C++ 关键字）。
 * -------------------------------------------------------------------------- */
typedef struct PwmInterface {
    int (*init)(void* instance);                                                /* 初始化：0 成功，负值失败 */
    int (*deinit)(void* instance);                                              /* 反初始化：0 成功，负值失败 */
    int (*configure)(void* instance, uint32_t channel, const PwmConfig* config); /* 配置通道频率与分辨率 */
    int (*setFrequency)(void* instance, uint32_t channel, uint32_t frequencyHz); /* 运行时改频率 */
    int (*setDuty)(void* instance, uint32_t channel, uint32_t duty);             /* 原始占空计数 0..(2^bits-1) */
    int (*setDutyPermille)(void* instance, uint32_t channel, uint32_t permille); /* 占空千分比 0..1000 */
    int (*start)(void* instance, uint32_t channel);                              /* 启动通道输出 */
    int (*stop)(void* instance, uint32_t channel);                               /* 停止通道输出 */
    void (*deleteInstance)(void* instance);                                      /* 【必需】销毁具体实例 */
} PwmInterface;

/* --------------------------------------------------------------------------
 * ② 抽象类结构体：只放「外部注入的具象实例」+ 虚表
 * -------------------------------------------------------------------------- */
typedef struct Pwm {
    void*         instance;    /* 指向具体实现的实例（多态核心） */
    PwmInterface  interfaces;  /* 接口函数表（字段名恒为 interfaces） */
} Pwm;

/* --------------------------------------------------------------------------
 * 接口表返回宏（供具象 .c 使用）
 *
 * 用法示例（具象 .c 内，返回 PwmInterface 的函数体中）：
 *     PwmInterface PwmEsp32c3Interface(void)
 *     {
 *         _RETURN_PWM_INTERFACE;                    // 函数名符合约定时
 *     }
 *     PwmInterface PwmEsp32c3Interface(void)
 *     {
 *         _RETURN_PWM_INTERFACE_WITH(ledcInit, ledcDeinit, ledcConfigure,
 *                                    ledcSetFrequency, ledcSetDuty,
 *                                    ledcSetDutyPermille, ledcStart,
 *                                    ledcStop, ledcDeleteInstance);  // 自定义函数名
 *     }
 *
 * 约定名（_RETURN_PWM_INTERFACE 展开时传入）：
 *     init / deinit / configure / setFrequency / setDuty /
 *     setDutyPermille / start / stop / deleteInstance
 *
 * 说明：宏以 return 语句结尾（不带分号），因此展开后函数体必然以 return 收尾，
 *       调用处结尾写 `;` 即可（多出的空语句合法）。
 * -------------------------------------------------------------------------- */
#define _RETURN_PWM_INTERFACE_WITH(initFn, deinitFn, configureFn, setFrequencyFn, setDutyFn, setDutyPermilleFn, startFn, stopFn, deleteFn) \
    PwmInterface interfaces = {                                       \
        .init            = (initFn),                                  \
        .deinit          = (deinitFn),                                \
        .configure       = (configureFn),                             \
        .setFrequency    = (setFrequencyFn),                          \
        .setDuty         = (setDutyFn),                               \
        .setDutyPermille = (setDutyPermilleFn),                       \
        .start           = (startFn),                                 \
        .stop            = (stopFn),                                  \
        .deleteInstance  = (deleteFn),                                \
    };                                                                \
    return interfaces

#define _RETURN_PWM_INTERFACE \
    _RETURN_PWM_INTERFACE_WITH(init, deinit, configure, setFrequency, setDuty, setDutyPermille, start, stop, deleteInstance)

/* --------------------------------------------------------------------------
 * ③ 抽象类构造 / 析构
 * NewPwm：只做「分配抽象对象 + 存 instance + 存 interfaces」，不创建具象实例；
 *         instance 或 interfaces.deleteInstance 为 NULL 时返回 NULL。
 * DeletePwm：先调 deleteInstance 销毁具象实例，再 pwmFree 抽象对象，
 *            最后把外部指针置 NULL。
 * -------------------------------------------------------------------------- */
Pwm* NewPwm(void* instance, PwmInterface interfaces);
void DeletePwm(Pwm** pPwm);

/* --------------------------------------------------------------------------
 * ④ 抽象接口包装：先判空 / 判非法入参，再透传给具象实现
 * -------------------------------------------------------------------------- */
int PwmInit(Pwm* pwm);
int PwmDeinit(Pwm* pwm);
int PwmConfigure(Pwm* pwm, uint32_t channel, const PwmConfig* config);
int PwmSetFrequency(Pwm* pwm, uint32_t channel, uint32_t frequencyHz);
int PwmSetDuty(Pwm* pwm, uint32_t channel, uint32_t duty);
int PwmSetDutyPermille(Pwm* pwm, uint32_t channel, uint32_t permille);
int PwmStart(Pwm* pwm, uint32_t channel);
int PwmStop(Pwm* pwm, uint32_t channel);

#if defined(__cplusplus)
}
#endif

#endif /* _PWM_H */
