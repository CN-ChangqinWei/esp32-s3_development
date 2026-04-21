#ifndef _GLOBAL_H
#define _GLOBAL_H

#include "serial.h"
#include "serial_comm.h"
#include "service.h"
#include "driver/gpio.h"

// ==================== GPIO 引脚映射定义 ====================

// UART 引脚映射
typedef struct {
    gpio_num_t tx_pin;      // TX 引脚
    gpio_num_t rx_pin;      // RX 引脚
    gpio_num_t rts_pin;     // RTS 引脚 (流控，可选)
    gpio_num_t cts_pin;     // CTS 引脚 (流控，可选)
    char use_flow_ctrl;  // 是否使用硬件流控
} UartPinMap;

// LED 引脚映射
typedef struct {
    gpio_num_t pin;         // LED 引脚
    char active_level;   // 有效电平 (0=低电平, 1=高电平)
} LedPinMap;

// 按键引脚映射
typedef struct {
    gpio_num_t pin;         // 按键引脚
    char active_level;   // 有效电平 (0=低电平, 1=高电平)
    char internal_pull;  // 是否使能内部上拉/下拉
} ButtonPinMap;

// 电机控制引脚映射 (示例)
typedef struct {
    gpio_num_t pwm_pin;     // PWM 控制引脚
    gpio_num_t dir_pin;     // 方向控制引脚
    gpio_num_t en_pin;      // 使能引脚
} MotorPinMap;

// 全局 GPIO 映射结构体
typedef struct {
    UartPinMap uart1;       // UART1 引脚映射 (通常用于调试)
    UartPinMap uart2;       // UART2 引脚映射 (通常用于通信)
    LedPinMap status_led;   // 状态 LED
    ButtonPinMap user_btn;  // 用户按键
    MotorPinMap motor;      // 电机控制
} GpioPinMap;

// 全局 GPIO 映射实例 (定义在 global.c)
extern GpioPinMap g_gpio_map;

// Serial 全局实例
extern Serial* serial1;
extern char sendBuf1[255];

// SerialComm 全局实例
extern Communication* serialComm;

// Service 全局实例
extern Service* g_service;

// ==================== 函数声明 ====================

// 全局初始化函数
void GlobalInit(void);

// GPIO 初始化函数
//void GpioInit(void);

// 获取 GPIO 映射配置
const GpioPinMap* GetGpioPinMap(void);

#endif
