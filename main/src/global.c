#include "global.h"
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "communication.h"
#include "serial_proto.h"
#include "router.h"
#include "health_comm.h"
#include "motor_comm.h"
#include "service.h"
#include "protocol.h"
#include "motor_serialize.h"
#include "json_proto.h"
static const char* TAG = "GLOBAL";

// ==================== GPIO 引脚映射配置 ====================
// 根据实际硬件修改以下引脚定义

GpioPinMap g_gpio_map = {
    // UART1 - 调试用 (TX=GPIO4, RX=GPIO5)
    .uart1 = {
        .tx_pin = GPIO_NUM_4,
        .rx_pin = GPIO_NUM_5,
        .rts_pin = GPIO_NUM_NC,     // 未使用
        .cts_pin = GPIO_NUM_NC,     // 未使用
        .use_flow_ctrl = 0
    },
    // UART2 - 通信用 (TX=GPIO17, RX=GPIO16)
    .uart2 = {
        .tx_pin = GPIO_NUM_17,
        .rx_pin = GPIO_NUM_16,
        .rts_pin = GPIO_NUM_NC,
        .cts_pin = GPIO_NUM_NC,
        .use_flow_ctrl = 0
    },
    // 状态 LED - GPIO2 (板载 LED)
    .status_led = {
        .pin = GPIO_NUM_2,
        .active_level = 1           // 高电平点亮
    },
    // 用户按键 - GPIO0 (BOOT 按键)
    .user_btn = {
        .pin = GPIO_NUM_0,
        .active_level = 0,          // 低电平有效
        .internal_pull = 1          // 使能内部上拉
    },
    // 电机控制引脚
    .motor = {
        .pwm_pin = GPIO_NUM_18,
        .dir_pin = GPIO_NUM_19,
        .en_pin = GPIO_NUM_21
    }
};

// Serial 全局实例定义
Serial* serial1 = NULL;
char sendBuf1[_SERIAL_BUF_SIZE] = {0};

// SerialComm 全局实例定义
SerialComm* serialComm = NULL;

// Service 全局实例定义
Service* g_service = NULL;

SerializeInterface serializeArray[NUM_OF_PROTO]={0};


// ==================== GPIO 初始化 ====================

// void GpioInit(void) {
//     ESP_LOGI(TAG, "Initializing GPIO pins...");
    
//     const GpioPinMap* map = &g_gpio_map;
    
//     // 1. 初始化状态 LED
//     gpio_config_t led_cfg = {
//         .pin_bit_mask = (1ULL << map->status_led.pin),
//         .mode = GPIO_MODE_OUTPUT,
//         .pull_up_en = GPIO_PULLUP_DISABLE,
//         .pull_down_en = GPIO_PULLDOWN_DISABLE,
//         .intr_type = GPIO_INTR_DISABLE
//     };
//     gpio_config(&led_cfg);
//     gpio_set_level(map->status_led.pin, !map->status_led.active_level);  // 默认关闭
//     ESP_LOGI(TAG, "Status LED pin %d configured", map->status_led.pin);
    
//     // 2. 初始化用户按键
//     gpio_config_t btn_cfg = {
//         .pin_bit_mask = (1ULL << map->user_btn.pin),
//         .mode = GPIO_MODE_INPUT,
//         .pull_up_en = map->user_btn.internal_pull ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
//         .pull_down_en = GPIO_PULLDOWN_DISABLE,
//         .intr_type = GPIO_INTR_DISABLE
//     };
//     gpio_config(&btn_cfg);
//     ESP_LOGI(TAG, "User button pin %d configured", map->user_btn.pin);
    
//     // 3. 初始化电机控制引脚
//     gpio_config_t motor_en_cfg = {
//         .pin_bit_mask = (1ULL << map->motor.en_pin) | (1ULL << map->motor.dir_pin),
//         .mode = GPIO_MODE_OUTPUT,
//         .pull_up_en = GPIO_PULLUP_DISABLE,
//         .pull_down_en = GPIO_PULLDOWN_DISABLE,
//         .intr_type = GPIO_INTR_DISABLE
//     };
//     gpio_config(&motor_en_cfg);
//     gpio_set_level(map->motor.en_pin, 0);    // 默认禁用电机
//     ESP_LOGI(TAG, "Motor pins configured: PWM=%d, DIR=%d, EN=%d", 
//              map->motor.pwm_pin, map->motor.dir_pin, map->motor.en_pin);
    
//     ESP_LOGI(TAG, "GPIO initialization completed");
// }

// 设置 UART 引脚
static void UartSetPins(uart_port_t uart_num, const UartPinMap* pin_map) {
    if (pin_map->tx_pin != GPIO_NUM_NC || pin_map->rx_pin != GPIO_NUM_NC) {
        esp_err_t err = uart_set_pin(uart_num, 
                                      pin_map->tx_pin, 
                                      pin_map->rx_pin, 
                                      pin_map->rts_pin, 
                                      pin_map->cts_pin);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to set UART%d pins", uart_num);
        } else {
            ESP_LOGI(TAG, "UART%d pins set: TX=%d, RX=%d", 
                     uart_num, pin_map->tx_pin, pin_map->rx_pin);
        }
    }
}

// ==================== 串口初始化 ====================

static void InitSerials(void) {
    const GpioPinMap* map = &g_gpio_map;
    
    // 默认配置：UART2, 9600, 8N1
    SerialConfig config = {
        .uart_num = UART_NUM_2,
        .rx_buffer_size = _SERIAL_DMA_BUF_SIZE,
        .tx_buffer_size = 0,
        .queue_size = 0,
        .uart_queue = NULL
    };

    config.uart_config.baud_rate = 9600;
    config.uart_config.data_bits = UART_DATA_8_BITS;
    config.uart_config.parity = UART_PARITY_DISABLE;
    config.uart_config.stop_bits = UART_STOP_BITS_1;
    config.uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    config.uart_config.source_clk = UART_SCLK_DEFAULT;

    serial1 = NewSerial(&config, _SERIAL_BUF_SIZE, sendBuf1, _SERIAL_BUF_SIZE);
    if (serial1 == NULL) {
        ESP_LOGE(TAG, "Failed to create serial1");
        return;
    }

    // 设置 UART 引脚映射
    UartSetPins(UART_NUM_2, &map->uart2);

    // 启动接收任务
    SerialStartRxTask(serial1, 5);
    ESP_LOGI(TAG, "Serial1 (UART2) initialized successfully");
}

// ==================== Service 初始化 ====================



static void ServiceInit(Service* service) {
    if (service == NULL) return;
    SerializeInterface motorSerializeInterface={
        MotorDomainSerialize,
        MotorDomainReserialize
    };
    serializeArray[PROTO_MOTOR]=motorSerializeInterface;
    // 创建 Communication 层
    Communication* comm = NewCommunicationFromSerial(serialComm);
    if (comm == NULL) {
        ESP_LOGE(TAG, "Failed to create communication");
        return;
    }

    // 创建 Protocol 层（包装 Communication）
    service->proto = NewJsonProto(comm, serializeArray, NUM_OF_PROTO);
    if (service->proto == NULL) {
        ESP_LOGE(TAG, "Failed to create json proto");
        DeleteCommunication(comm);
        return;
    }

    // 发送 hello
    MotorDomain data={
        .protocol=PROTO_MOTOR,
        .numAngel=100,
        .denAngel=180
    };
    ProtoSendPackage(service->proto, (char*)&data, sizeof(MotorDomain));

    service->router = NewRouter();
    if (service->router != NULL) {
        RouterHandlerPkg healthHandler = {HealthCommHandler, service};
        RouterRegister(service->router, Health, healthHandler);
        RouterHandlerPkg motorHandler = {MotorHandler, service};
        RouterRegister(service->router, PROTO_MOTOR, motorHandler);
        RouterHandlerPkg errHandler = {ServiceErrHandler, service};
        RouterSetErrHandler(service->router, errHandler);
        RouterStart(service->router);
    }
    ServiceStart(service);
}

// ==================== 全局初始化入口 ====================

void GlobalInit(void) {
    ESP_LOGI(TAG, "Starting global initialization...");

    // 1. 初始化 GPIO 引脚
    //GpioInit();

    // 2. 初始化串口
    InitSerials();

    // 3. 创建 SerialComm
    if (serial1 != NULL) {
        serialComm = NewSerialComm(serial1);
    }

    // 4. 创建并初始化 Service
    g_service = NewService();
    if (g_service != NULL) {
        ServiceInit(g_service);
    }

    ESP_LOGI(TAG, "Global initialization completed");
}

// ==================== 获取 GPIO 映射 ====================

const GpioPinMap* GetGpioPinMap(void) {
    return &g_gpio_map;
}
