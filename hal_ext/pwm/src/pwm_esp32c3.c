/**
 * @file pwm_esp32c3.c
 * @brief Pwm 抽象父类（pwm.h）的 ESP32-C3 具象派生实现（ESP-IDF v5.5.x / LEDC）。
 *
 * ============================================================
 * 文件结构
 * ============================================================
 *   ① 平台常量 / 引脚合法性兜底
 *   ② 通道 ↔ 定时器映射与入参校验（纯函数，无副作用）
 *   ③ 虚表接口实现（全部 static，首参恒为 void* instance，首行取 self）
 *   ④ 接口表 PwmEsp32c3Interface（_RETURN_PWM_INTERFACE_WITH）
 *   ⑤ 具体构造 NewPwmEsp32c3（内部必须调用抽象构造 NewPwm 完成装配）
 *
 * ============================================================
 * ESP32-C3 LEDC 资源模型
 * ============================================================
 *   速度模式：C3 只有低速模式，固定 LEDC_LOW_SPEED_MODE
 *   通道    ：0..5（LEDC_CHANNEL_0..5）
 *   定时器  ：0..3（LEDC_TIMER_0..3）
 *   映射    ：通道 i < 4 → 定时器 i（独占）
 *             通道 i = 4/5 → 定时器 i-4（与通道 0/1 共用）
 *   分辨率  ：上限 LEDC_TIMER_14_BIT（14 位）
 *
 *   共用定时器的两路通道必然同频、同分辨率。因此 configure() 发现该定时器
 *   已被「别的通道」配置过且频率/分辨率不一致时返回 -1，不静默改动他人配置；
 *   同理 setFrequency() 改的是定时器频率，会影响共用该定时器的另一路通道
 *   （硬件限制，调用方需自行确认）。
 *
 * ============================================================
 * 返回值与错误处理
 * ============================================================
 *   本模块一律用 0 成功 / -1 失败（含底层 ledc 驱动返回的任何非 ESP_OK）。
 *   抽象层保留 -2 表示「虚表槽位未实现」，具象层不返回 -2。
 *
 * ============================================================
 * 内存与依赖约定
 * ============================================================
 *   ○ 对象内存一律走抽象层的 pwmMalloc / pwmFree（默认 malloc / free，
 *     可被外部替换为 pvPortMalloc / vPortFree）；
 *   ○ 本文件是唯一允许出现 ESP-IDF 类型的地方，抽象头 / 具象头均保持纯净。
 */

#include "pwm_esp32c3.h"

#include <string.h>

#include "driver/ledc.h"
#include "esp_err.h"

/* ==================================================================
 * ① 平台常量
 * ================================================================== */

/* ESP32-C3 只有低速模式：LEDC_HIGH_SPEED_MODE 在该芯片上不存在 */
#define PWM_ESP32C3_LEDC_MODE   LEDC_LOW_SPEED_MODE

/* 引脚合法性兜底：若当前包含链没有暴露 IDF 的 GPIO_IS_VALID_GPIO，
 * 则按 ESP32-C3 的 0..21 自行校验（与 hal/gpio 的兜底方式保持一致）。 */
#ifndef GPIO_IS_VALID_GPIO
#define GPIO_IS_VALID_GPIO(gpio) \
    (((int)(gpio) >= 0) && ((int)(gpio) < (int)PWM_ESP32C3_GPIO_COUNT))
#endif

/* ==================================================================
 * ② 通道 ↔ 定时器映射与入参校验
 * ================================================================== */

/**
 * @brief 通道号 -> LEDC 定时器号。
 *
 * 通道 0..3 独占同名定时器；通道 4/5 复用定时器 0/1
 * （ESP32-C3 只有 4 个定时器，6 个通道必然有两组共享）。
 */
static uint32_t pwmEsp32c3TimerOf(uint32_t channel)
{
    return (channel < (uint32_t)PWM_ESP32C3_TIMER_SHARE_BASE)
               ? channel
               : (channel - (uint32_t)PWM_ESP32C3_TIMER_SHARE_BASE);
}

/**
 * @brief 查找与该通道共用同一定时器、且已配置成功的「别的通道」。
 *
 * @return 找到返回该通道号（>= 0）；没有则返回 -1。
 */
static int32_t pwmEsp32c3FindTimerPeer(const PwmEsp32c3* self, uint32_t channel)
{
    uint32_t timer = 0u;
    uint32_t i     = 0u;

    if (self == NULL) {
        return -1;
    }

    timer = pwmEsp32c3TimerOf(channel);
    for (i = 0u; i < self->channelCount; i++) {
        if (i == channel) {
            continue;
        }
        if (0u == self->channelConfigured[i]) {
            continue;
        }
        if (pwmEsp32c3TimerOf(i) == timer) {
            return (int32_t)i;
        }
    }

    return -1;
}

/**
 * @brief 实例本身是否可用：非空且通道数落在 1..6。
 *
 * 具象头暴露了结构体定义，instance 也可能由外部注入（依赖反转场景），
 * 因此每个接口都要先确认 channelCount 合法，避免后续数组访问越界。
 */
static int pwmEsp32c3CheckInstance(const PwmEsp32c3* self)
{
    if (self == NULL) {
        return PWM_ESP32C3_ERR_PARAM;
    }
    if (0u == self->channelCount ||
        self->channelCount > (uint32_t)PWM_ESP32C3_CHANNEL_COUNT) {
        return PWM_ESP32C3_ERR_PARAM;
    }
    return PWM_ESP32C3_OK;
}

/** @brief 实例 + 通道号校验（通道越界属于具象层职责，抽象层不知平台通道数）。 */
static int pwmEsp32c3CheckChannel(const PwmEsp32c3* self, uint32_t channel)
{
    if (pwmEsp32c3CheckInstance(self) != PWM_ESP32C3_OK) {
        return PWM_ESP32C3_ERR_PARAM;
    }
    if (channel >= self->channelCount) {
        return PWM_ESP32C3_ERR_PARAM;
    }
    return PWM_ESP32C3_OK;
}

/**
 * @brief 实例 + 通道号 + 「该通道已 configure」校验。
 *
 * 未配置的通道其定时器 / 分辨率均未知（frequencyHz、resolutionBits 为 0），
 * 此时改频率、算占空比都没有意义，一律拒绝。
 */
static int pwmEsp32c3CheckConfigured(const PwmEsp32c3* self, uint32_t channel)
{
    if (pwmEsp32c3CheckChannel(self, channel) != PWM_ESP32C3_OK) {
        return PWM_ESP32C3_ERR_PARAM;
    }
    if (0u == self->channelConfigured[channel]) {
        return PWM_ESP32C3_ERR_PARAM;
    }
    return PWM_ESP32C3_OK;
}

/* ==================================================================
 * ③ 虚表接口实现（static，首参恒为 void* instance）
 * ================================================================== */

/**
 * @brief 初始化：LEDC 没有全局初始化步骤，只复位软件侧运行时记录。
 *
 * 定时器 / 通道都在 configure() 时按需配置，init 不占用硬件资源；
 * 但若已有通道处于已配置状态，则拒绝复位（否则软件记录与硬件状态脱节，
 * deinit 将无法停止这些通道），因此「仅有构造后未配置」时才幂等成功。
 */
static int pwmEsp32c3Init(void* instance)
{
    PwmEsp32c3* self = (PwmEsp32c3*)instance;
    uint32_t    i    = 0u;

    if (pwmEsp32c3CheckInstance(self) != PWM_ESP32C3_OK) {
        return PWM_ESP32C3_ERR_PARAM;
    }

    /* 引脚表兜底校验：instance 可能由外部注入，构造期的校验不能完全依赖 */
    for (i = 0u; i < self->channelCount; i++) {
        if (!GPIO_IS_VALID_GPIO((int)self->pins[i])) {
            return PWM_ESP32C3_ERR_PARAM;
        }
    }

    /* 已存在运行中的通道：拒绝复位，避免丢失停止通道所需的记录 */
    for (i = 0u; i < self->channelCount; i++) {
        if (0u != self->channelConfigured[i]) {
            return PWM_ESP32C3_ERR_PARAM;
        }
    }

    for (i = 0u; i < (uint32_t)PWM_ESP32C3_TIMER_COUNT; i++) {
        self->frequencyHz[i] = 0u;
    }
    for (i = 0u; i < (uint32_t)PWM_ESP32C3_CHANNEL_COUNT; i++) {
        self->resolutionBits[i]    = 0u;
        self->channelConfigured[i] = 0u;
    }

    return PWM_ESP32C3_OK;
}

/**
 * @brief 配置通道的频率与分辨率（ledc_timer_config + ledc_channel_config）。
 *
 * 冲突规则：若该通道所属定时器已被「别的通道」配置过，
 *           则本次 frequencyHz / resolutionBits 必须与之一致，否则返回 -1；
 *           同一通道自身重复 configure 允许改参数（此时无别的通道受影响）。
 *
 * 占空比初始化为 0（hpoint = 0），通道输出从「常低」开始；
 * 之后用 setDuty / setDutyPermille 设定占空，用 start / stop 控制输出。
 */
static int pwmEsp32c3Configure(void* instance, uint32_t channel, const PwmConfig* config)
{
    PwmEsp32c3*           self          = (PwmEsp32c3*)instance;
    ledc_timer_config_t   timerConfig;
    ledc_channel_config_t channelConfig;
    int32_t               peer          = -1;
    uint32_t              timer         = 0u;
    esp_err_t             err           = ESP_OK;

    if (self == NULL || config == NULL) {
        return PWM_ESP32C3_ERR_PARAM;
    }
    if (pwmEsp32c3CheckChannel(self, channel) != PWM_ESP32C3_OK) {
        return PWM_ESP32C3_ERR_PARAM;
    }
    if (0u == config->frequencyHz) {
        return PWM_ESP32C3_ERR_PARAM;
    }
    /* 分辨率上限是平台相关量（C3 为 14 位），由具象层判定 */
    if (0u == config->resolutionBits ||
        config->resolutionBits > (uint8_t)PWM_ESP32C3_MAX_RESOLUTION_BITS) {
        return PWM_ESP32C3_ERR_PARAM;
    }
    if (!GPIO_IS_VALID_GPIO((int)self->pins[channel])) {
        return PWM_ESP32C3_ERR_PARAM;
    }

    timer = pwmEsp32c3TimerOf(channel);

    /* 定时器共享冲突检查：禁止静默改动已配置通道的频率 / 分辨率 */
    peer = pwmEsp32c3FindTimerPeer(self, channel);
    if (peer >= 0) {
        if (self->resolutionBits[peer] != config->resolutionBits ||
            self->frequencyHz[timer] != config->frequencyHz) {
            return PWM_ESP32C3_ERR_PARAM;
        }
    }

    /* 定时器：低速模式 + 自动时钟源 + 分辨率 / 频率 */
    memset(&timerConfig, 0, sizeof(timerConfig));
    timerConfig.speed_mode      = PWM_ESP32C3_LEDC_MODE;
    timerConfig.duty_resolution = (ledc_timer_bit_t)config->resolutionBits;
    timerConfig.timer_num       = (ledc_timer_t)timer;
    timerConfig.freq_hz         = config->frequencyHz;
    timerConfig.clk_cfg         = LEDC_AUTO_CLK;

    err = ledc_timer_config(&timerConfig);
    if (ESP_OK != err) {
        return PWM_ESP32C3_ERR_PARAM;
    }

    /* 通道：绑定引脚与本定时器，duty / hpoint 从 0 开始，不使用 LEDC 中断 */
    memset(&channelConfig, 0, sizeof(channelConfig));
    channelConfig.gpio_num   = (int)self->pins[channel];
    channelConfig.speed_mode = PWM_ESP32C3_LEDC_MODE;
    channelConfig.channel    = (ledc_channel_t)channel;
    channelConfig.intr_type  = LEDC_INTR_DISABLE;
    channelConfig.timer_sel  = (ledc_timer_t)timer;
    channelConfig.duty       = 0u;
    channelConfig.hpoint     = 0;

    err = ledc_channel_config(&channelConfig);
    if (ESP_OK != err) {
        return PWM_ESP32C3_ERR_PARAM;
    }

    /* 只有两步都成功，才记录配置结果 */
    self->frequencyHz[timer]         = config->frequencyHz;
    self->resolutionBits[channel]    = config->resolutionBits;
    self->channelConfigured[channel] = 1u;

    return PWM_ESP32C3_OK;
}

/**
 * @brief 运行时改频率（改的是定时器频率）。
 *
 * @note 共用定时器的另一路通道会同步变频（LEDC 硬件限制）；
 *       驱动返回的任何错误（如 ESP_ERR_INVALID_ARG / ESP_ERR_INVALID_STATE）
 *       统一折叠为 -1。
 */
static int pwmEsp32c3SetFrequency(void* instance, uint32_t channel, uint32_t frequencyHz)
{
    PwmEsp32c3* self  = (PwmEsp32c3*)instance;
    uint32_t    timer = 0u;
    esp_err_t   err   = ESP_OK;

    if (pwmEsp32c3CheckConfigured(self, channel) != PWM_ESP32C3_OK) {
        return PWM_ESP32C3_ERR_PARAM;
    }
    if (0u == frequencyHz) {
        return PWM_ESP32C3_ERR_PARAM;
    }

    timer = pwmEsp32c3TimerOf(channel);

    err = ledc_set_freq(PWM_ESP32C3_LEDC_MODE, (ledc_timer_t)timer, frequencyHz);
    if (ESP_OK != err) {
        return PWM_ESP32C3_ERR_PARAM;
    }

    self->frequencyHz[timer] = frequencyHz;

    return PWM_ESP32C3_OK;
}

/**
 * @brief 设置原始占空计数（0 .. 2^bits - 1）。
 *
 * 合法上限取决于该通道 configure() 时记录的分辨率位数，
 * 因此未配置的通道一律返回 -1。
 */
static int pwmEsp32c3SetDuty(void* instance, uint32_t channel, uint32_t duty)
{
    PwmEsp32c3* self     = (PwmEsp32c3*)instance;
    uint32_t    maxDuty  = 0u;
    esp_err_t   err      = ESP_OK;

    if (pwmEsp32c3CheckConfigured(self, channel) != PWM_ESP32C3_OK) {
        return PWM_ESP32C3_ERR_PARAM;
    }

    maxDuty = (1u << (uint32_t)self->resolutionBits[channel]) - 1u;
    if (duty > maxDuty) {
        return PWM_ESP32C3_ERR_PARAM;
    }

    err = ledc_set_duty(PWM_ESP32C3_LEDC_MODE, (ledc_channel_t)channel, duty);
    if (ESP_OK != err) {
        return PWM_ESP32C3_ERR_PARAM;
    }

    err = ledc_update_duty(PWM_ESP32C3_LEDC_MODE, (ledc_channel_t)channel);
    if (ESP_OK != err) {
        return PWM_ESP32C3_ERR_PARAM;
    }

    return PWM_ESP32C3_OK;
}

/**
 * @brief 设置占空千分比（0..1000‰），换算在本具象层完成。
 *
 *   duty = permille * (2^bits - 1) / 1000
 *
 * permille 最大 1000、maxDuty 最大 16383，乘积 16_383_000 不会溢出 uint32。
 */
static int pwmEsp32c3SetDutyPermille(void* instance, uint32_t channel, uint32_t permille)
{
    PwmEsp32c3* self    = (PwmEsp32c3*)instance;
    uint32_t    maxDuty = 0u;
    uint32_t    duty    = 0u;

    if (pwmEsp32c3CheckConfigured(self, channel) != PWM_ESP32C3_OK) {
        return PWM_ESP32C3_ERR_PARAM;
    }
    if (permille > (uint32_t)PWM_ESP32C3_PERMILLE_MAX) {
        return PWM_ESP32C3_ERR_PARAM;
    }

    maxDuty = (1u << (uint32_t)self->resolutionBits[channel]) - 1u;
    duty    = (permille * maxDuty) / (uint32_t)PWM_ESP32C3_PERMILLE_MAX;

    /* 复用 setDuty：越界与驱动错误处理保持一致 */
    return pwmEsp32c3SetDuty(instance, channel, duty);
}

/**
 * @brief 启动通道输出：恢复定时器计数 + 重新使能该通道信号输出。
 *
 * ledc_timer_resume 让定时器的占空计数继续运行（对未暂停的定时器是幂等的），
 * ledc_update_duty 重新打开该通道的 signal_out 并触发占空更新，
 * 因此可与 stop()（ledc_stop 关闭 signal_out）成对使用。
 */
static int pwmEsp32c3Start(void* instance, uint32_t channel)
{
    PwmEsp32c3* self  = (PwmEsp32c3*)instance;
    uint32_t    timer = 0u;
    esp_err_t   err   = ESP_OK;

    if (pwmEsp32c3CheckConfigured(self, channel) != PWM_ESP32C3_OK) {
        return PWM_ESP32C3_ERR_PARAM;
    }

    timer = pwmEsp32c3TimerOf(channel);

    err = ledc_timer_resume(PWM_ESP32C3_LEDC_MODE, (ledc_timer_t)timer);
    if (ESP_OK != err) {
        return PWM_ESP32C3_ERR_PARAM;
    }

    err = ledc_update_duty(PWM_ESP32C3_LEDC_MODE, (ledc_channel_t)channel);
    if (ESP_OK != err) {
        return PWM_ESP32C3_ERR_PARAM;
    }

    return PWM_ESP32C3_OK;
}

/**
 * @brief 停止通道输出。
 *
 * 采用通道粒度的 ledc_stop(speed_mode, channel, idle_level = 0)，
 * 而不是 ledc_timer_pause(speed_mode, timer)：
 *   通道 4/5 与通道 0/1 共用定时器，按定时器暂停会连带停掉共享它的另一路通道；
 *   而 stop 的语义是「停止这一路输出」，故必须用通道粒度接口。
 * ledc_stop 会把该通道 signal_out 关闭并保持 idle 电平，之后 start() 可恢复。
 */
static int pwmEsp32c3Stop(void* instance, uint32_t channel)
{
    PwmEsp32c3* self = (PwmEsp32c3*)instance;
    esp_err_t   err  = ESP_OK;

    if (pwmEsp32c3CheckConfigured(self, channel) != PWM_ESP32C3_OK) {
        return PWM_ESP32C3_ERR_PARAM;
    }

    err = ledc_stop(PWM_ESP32C3_LEDC_MODE, (ledc_channel_t)channel, 0u);
    if (ESP_OK != err) {
        return PWM_ESP32C3_ERR_PARAM;
    }

    return PWM_ESP32C3_OK;
}

/**
 * @brief 反初始化：停止全部已配置通道并清空软件记录（幂等，可再次 configure）。
 *
 * 注：只停止 LEDC 输出，不把 GPIO 复位成输入 / 上拉（那是 gpio 模块的职责，
 *     本模块不越界改动引脚电气属性）。
 */
static int pwmEsp32c3Deinit(void* instance)
{
    PwmEsp32c3* self = (PwmEsp32c3*)instance;
    uint32_t    i    = 0u;

    if (pwmEsp32c3CheckInstance(self) != PWM_ESP32C3_OK) {
        return PWM_ESP32C3_ERR_PARAM;
    }

    for (i = 0u; i < self->channelCount; i++) {
        if (0u == self->channelConfigured[i]) {
            continue;
        }
        (void)ledc_stop(PWM_ESP32C3_LEDC_MODE, (ledc_channel_t)i, 0u);
    }

    for (i = 0u; i < (uint32_t)PWM_ESP32C3_TIMER_COUNT; i++) {
        self->frequencyHz[i] = 0u;
    }
    for (i = 0u; i < (uint32_t)PWM_ESP32C3_CHANNEL_COUNT; i++) {
        self->resolutionBits[i]    = 0u;
        self->channelConfigured[i] = 0u;
    }

    return PWM_ESP32C3_OK;
}

/**
 * @brief 销毁具体实例：先 deinit（停止已配置通道）再 pwmFree。
 *
 * 只由抽象析构 DeletePwm 经虚表调用一次；pwmFree 后局部指针置空防悬空。
 */
static void pwmEsp32c3DeleteInstance(void* instance)
{
    PwmEsp32c3* self = (PwmEsp32c3*)instance;

    if (self == NULL) {
        return;
    }

    (void)pwmEsp32c3Deinit((void*)self);

    if (pwmFree != NULL) {
        pwmFree(self);
    }
    self = NULL; /* 局部指针置空，防止释放后误用 */
}

/* ==================================================================
 * ④ 接口表（字段顺序严格对应 pwm.h 的 PwmInterface）
 * ================================================================== */
PwmInterface PwmEsp32c3Interface(void)
{
    _RETURN_PWM_INTERFACE_WITH(pwmEsp32c3Init,
                               pwmEsp32c3Deinit,
                               pwmEsp32c3Configure,
                               pwmEsp32c3SetFrequency,
                               pwmEsp32c3SetDuty,
                               pwmEsp32c3SetDutyPermille,
                               pwmEsp32c3Start,
                               pwmEsp32c3Stop,
                               pwmEsp32c3DeleteInstance);
}

/* ==================================================================
 * ⑤ 具体构造（内部必须调用抽象构造 NewPwm 完成装配）
 *    instance / interfaces 只能由 NewPwm 赋值，禁止在本文件手工写
 *    res->instance = self;
 * ================================================================== */
Pwm* NewPwmEsp32c3(const uint32_t* gpioPins, uint32_t channelCount)
{
    PwmEsp32c3* self = NULL;
    Pwm*        pwm  = NULL;
    uint32_t    i    = 0u;
    uint32_t    j    = 0u;

    /* ① 参数校验：引脚表非空、通道数 1..6 */
    if (gpioPins == NULL) {
        return NULL;
    }
    if (0u == channelCount || channelCount > (uint32_t)PWM_ESP32C3_CHANNEL_COUNT) {
        return NULL;
    }

    /* ② 引脚校验：合法 GPIO（0..21）且不允许两路复用同一引脚 */
    for (i = 0u; i < channelCount; i++) {
        if (!GPIO_IS_VALID_GPIO((int)gpioPins[i])) {
            return NULL;
        }
        for (j = 0u; j < i; j++) {
            if (gpioPins[j] == gpioPins[i]) {
                return NULL;
            }
        }
    }

    if (pwmMalloc == NULL) {
        return NULL;
    }

    /* ③ 分配具体实例并清零（未使用的引脚槽位为 0，状态字段为「未配置」） */
    self = (PwmEsp32c3*)pwmMalloc(sizeof(PwmEsp32c3));
    if (self == NULL) {
        return NULL;
    }
    memset(self, 0, sizeof(PwmEsp32c3));

    /* ④ 保存引脚表与通道数（构造阶段不占用 LEDC 资源） */
    for (i = 0u; i < channelCount; i++) {
        self->pins[i] = gpioPins[i];
    }
    self->channelCount = channelCount;

    /* ⑤ 抽象构造装配：只负责「分配抽象对象 + 存 instance + 存 interfaces」 */
    pwm = NewPwm((void*)self, PwmEsp32c3Interface());
    if (pwm == NULL) {
        if (pwmFree != NULL) {
            pwmFree(self);
        }
        self = NULL;
        return NULL;
    }

    return pwm;
}
