#ifndef _PWM_ESP32C3_H
#define _PWM_ESP32C3_H

/* ==================================================================
 * pwm_esp32c3.h —— PWM 抽象父类（pwm.h）的 ESP32-C3 具象派生类（LEDC）
 *
 * OOP in C 关系：
 *   抽象父类 Pwm（pwm.h）
 *       { void* instance; PwmInterface interfaces; }
 *                │ instance
 *                ▼
 *   具象派生类 PwmEsp32c3（本文件）
 *       { 引脚表 / 定时器维度频率与分辨率 / 通道配置标志 / 通道数 }
 *
 *   上层（main / service）只依赖 pwm.h 的 Pwm*，通过 PwmInit / PwmConfigure /
 *   PwmSetDutyPermille ... 等包装函数调用；运行时真正执行的是
 *   pwm_esp32c3.c 内的静态接口实现（pwmEsp32c3Init / ... / pwmEsp32c3DeleteInstance）。
 *
 * 头文件约束（强制）：
 *   1. 本文件只包含抽象父类 pwm.h 与 <stdint.h>，
 *      禁止包含 driver/*、freertos/*、esp_*.h 等平台头，
 *      保证使用方不被拖入平台头文件链（hal 组件用 PRIV_REQUIRES 隐藏驱动依赖）；
 *   2. 结构体字段只用中性整型（uint32_t / uint8_t），平台类型
 *      （ledc_timer_t / ledc_channel_t / ledc_timer_config_t / esp_err_t …）
 *      只允许出现在 pwm_esp32c3.c 内部。
 *
 * ESP32-C3 LEDC 平台映射：
 *   speed_mode      : 固定 LEDC_LOW_SPEED_MODE（C3 只有低速模式）
 *   channel         : 0..5（LEDC_CHANNEL_0..5），对应构造时 gpioPins[] 的下标
 *   timer           : 0..3（LEDC_TIMER_0..3）
 *   通道 -> 定时器   : i < 4 → 定时器 i（独占）；i = 4/5 → 定时器 i-4（复用）
 *   分辨率上限       : LEDC_TIMER_14_BIT（14 位）
 *
 *   注意：共用定时器的两路通道必然同频、同分辨率（LEDC 硬件限制），
 *   因此 configure() 中若发现该定时器已被「别的通道」配置过且频率/分辨率
 *   不一致，直接返回 -1，绝不静默改动已配置通道。
 *
 * 返回值约定（与 pwm.h 一致）：
 *    0  : 成功
 *   -1 : 参数非法 / 通道越界 / 分辨率越界 / 千分比越界 /
 *        定时器参数冲突 / 底层 ledc 驱动返回错误
 *   -2 : 抽象层保留（该虚表槽位未实现），具象层不返回
 *
 * 用法：
 *    static const uint32_t pins[2] = {4u, 5u};
 *    Pwm* pwm = NewPwmEsp32c3(pins, 2u);          // 具体构造（失败返回 NULL）
 *    PwmConfig config = { .frequencyHz = 5000u, .resolutionBits = 10u };
 *    PwmInit(pwm);
 *    PwmConfigure(pwm, 0u, &config);              // 配置通道 0（定时器 0）
 *    PwmSetDutyPermille(pwm, 0u, 500u);           // 50% 占空
 *    PwmStart(pwm, 0u);
 *    PwmStop(pwm, 0u);
 *    DeletePwm(&pwm);                             // 内部经 deleteInstance 释放
 * ================================================================== */

#include <stdint.h>

#include "pwm.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==================================================================
 * ESP32-C3 LEDC 硬件上限（纯数值，无平台类型）
 * ================================================================== */
#define PWM_ESP32C3_CHANNEL_COUNT         (6)   /* 通道 0..5 */
#define PWM_ESP32C3_TIMER_COUNT           (4)   /* 定时器 0..3 */
#define PWM_ESP32C3_MAX_RESOLUTION_BITS   (14)  /* LEDC_TIMER_14_BIT */
#define PWM_ESP32C3_TIMER_SHARE_BASE      (4)   /* 通道 >= 4 复用定时器 (i - 4) */
#define PWM_ESP32C3_GPIO_COUNT            (22)  /* ESP32-C3 有效 GPIO：0..21 */
#define PWM_ESP32C3_PERMILLE_MAX          (1000u) /* 千分比满量程 */

/* 模块私有返回码（与 pwm.h 的 0 / -1 / -2 约定保持一致） */
#define PWM_ESP32C3_OK                    (0)
#define PWM_ESP32C3_ERR_PARAM             (-1)

/* ==================================================================
 * 具象派生类实例结构体（字段只需中性整型；由 pwm_esp32c3.c 填充与解释）
 *
 *   pins[channel]              : 第 channel 路 PWM 的输出 GPIO 编号（构造时固定）
 *   frequencyHz[timer]         : 定时器当前频率；0 = 该定时器尚未配置
 *   resolutionBits[channel]    : 通道已配置的分辨率位数；0 = 该通道尚未配置
 *                                （共用定时器的通道取值必然相同；permille 换算用它）
 *   channelConfigured[channel] : 1 = configure() 成功过（deinit / stop 的判据）
 *   channelCount               : 本实例实际使用的通道数，1..6
 * ================================================================== */
typedef struct PwmEsp32c3 {
    uint32_t pins[PWM_ESP32C3_CHANNEL_COUNT];              /* 通道 -> 输出引脚 */
    uint32_t frequencyHz[PWM_ESP32C3_TIMER_COUNT];         /* 定时器 -> 当前频率 */
    uint8_t  resolutionBits[PWM_ESP32C3_CHANNEL_COUNT];    /* 通道 -> 分辨率位数 */
    uint8_t  channelConfigured[PWM_ESP32C3_CHANNEL_COUNT]; /* 通道 -> 已配置标志 */
    uint32_t channelCount;                                 /* 已启用通道数，1..6 */
} PwmEsp32c3;

/* ==================================================================
 * 具体构造 / 虚表
 * ================================================================== */

/**
 * @brief 具体构造：创建 ESP32-C3 的 PWM（LEDC）对象。
 *
 * 构造流程（硬件按需延迟到 configure，构造阶段不占用任何 LEDC 资源）：
 *   pwmMalloc → memset → 保存引脚表与通道数 → 校验 →
 *   NewPwm((void*)self, PwmEsp32c3Interface()) 完成抽象装配。
 * 装配失败会 pwmFree(self) 并返回 NULL，绝不泄漏具象实例。
 *
 * @param gpioPins     引脚表，长度必须 >= channelCount；
 *                     gpioPins[i] = 第 i 路 PWM 的输出 GPIO（0..21，且不得重复）。
 * @param channelCount 通道数，合法范围 1..6。
 *
 * @return 成功返回抽象父类指针 Pwm*（由 DeletePwm 统一销毁）；
 *         gpioPins 为 NULL、channelCount 为 0 / 大于 6、引脚非法或重复、
 *         内存不足、抽象装配失败时返回 NULL。
 *
 * @note 通道 i 使用 LEDC 通道 i：i < 4 独占定时器 i，i = 4/5 复用定时器 i-4。
 */
Pwm* NewPwmEsp32c3(const uint32_t* gpioPins, uint32_t channelCount);

/**
 * @brief 返回 ESP32-C3 具象实现的接口表（虚函数表）。
 *
 * 字段顺序严格对应 pwm.h 的 PwmInterface：
 *     init / deinit / configure / setFrequency / setDuty /
 *     setDutyPermille / start / stop / deleteInstance
 *
 * @return 填满全部函数指针的 PwmInterface（deleteInstance 保证非 NULL）。
 */
PwmInterface PwmEsp32c3Interface(void);

#ifdef __cplusplus
}
#endif

#endif /* _PWM_ESP32C3_H */
