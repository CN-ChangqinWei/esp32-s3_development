#include "global.h"
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "communication.h"
#include "proto.h"
#include "serial_proto.h"
#include "task_que.h"
#include "router.h"
#include "health_comm.h"
#include "motor_comm.h"
#include "service.h"
#include "protocol.h"
#include "motor_serialize.h"
#include "json_proto.h"
#include "my_wifi.h"
#include "mqtt_comm.h"
static const char* TAG = "GLOBAL";
#define WIFI_SSD    "荣耀畅玩40"
#define WIFI_PSWD   "12345678"
#define MQTT_URL    "mqtt://120.53.247.129:9091"
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


// Service 全局实例定义
Service* g_uartService = NULL;      // UART服务
Service* g_mqttService = NULL;      // MQTT服务

// UART协议序列化接口
SerializeInterface uartSerializeArray[NUM_OF_PROTO]={0};
// MQTT协议序列化接口
SerializeInterface mqttSerializeArray[NUM_OF_PROTO]={0};

Communication* mqttComm = NULL;
Communication* serialComm = NULL;

// TaskQue 全局实例
TaskQue* g_uartTaskQue = NULL;      // UART服务的任务队列
TaskQue* g_mqttTaskQue = NULL;      // MQTT服务的任务队列

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

static void GlobalWifiInit(){
    
    int err=WifiInit(WIFI_SSD,WIFI_PSWD);

}

static void MqttInit(){
    MqttConfig config={
        .username="dev0",
        .pub_topic="dev0_pub",
        .sub_topic="dev0",
        .uri=MQTT_URL,
        .client_id="dev0",
        
    };
    mqttComm = NewMqttComm(&config);
    if(mqttComm==NULL){
        ESP_LOGE(TAG,"Fail to new mqttcomm");
    }
    while(!MqttCommIsConnected(mqttComm))  vTaskDelay(pdMS_TO_TICKS(10));
}

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
    
    // 创建 SerialComm 通信层
    serialComm = NewSerialComm(serial1);
    if (serialComm == NULL) {
        ESP_LOGE(TAG, "Failed to create SerialComm");
    }
}

// ==================== UART Service 初始化 ====================

static void UartServiceInit(Service* service) {
    if (service == NULL) return;
    
    ESP_LOGI(TAG, "Initializing UART service...");
    
    // 配置序列化接口（根据实际需求配置）
    SerializeInterface motorSerializeInterface = {
        MotorDomainSerialize,
        MotorDomainReserialize
    };
    uartSerializeArray[PROTO_MOTOR] = motorSerializeInterface;
    
    // 创建 SerialComm 层
    if (serialComm == NULL) {
        ESP_LOGE(TAG, "SerialComm not initialized");
        return;
    }
    
    // 创建 Protocol 层 - SerialProto（二进制协议）
    SerialProto* serialProto = NewSerialProto(serialComm);
    if (serialProto == NULL) {
        ESP_LOGE(TAG, "Failed to create serial proto");
        return;
    }
    service->proto = NewProto(serialProto, SerialProtoInterface());
    if (service->proto == NULL) {
        ESP_LOGE(TAG, "Failed to create proto wrapper");
        DeleteSerialProto(serialProto);
        return;
    }

    // 创建并启动 TaskQue
    g_uartTaskQue = NewTaskQue(100);
    if (g_uartTaskQue == NULL) {
        ESP_LOGE(TAG, "Failed to create UART task queue");
        return;
    }
    TaskQueStart(g_uartTaskQue, -1);
    
    // 创建 Router，注入 TaskQue
    service->router = NewRouter(g_uartTaskQue);
    if (service->router != NULL) {
        RouterHandlerPkg healthHandler = {HealthCommHandler, service};
        RouterRegister(service->router, Health, healthHandler);
        RouterHandlerPkg motorHandler = {MotorHandler, service};
        RouterRegister(service->router, PROTO_MOTOR, motorHandler);
        RouterHandlerPkg errHandler = {ServiceErrHandler, service};
        RouterSetErrHandler(service->router, errHandler);
    }
    
    ServiceStart(service);
    ESP_LOGI(TAG, "UART service started");
}

// ==================== MQTT Service 初始化 ====================

static void MqttServiceInit(Service* service) {
    if (service == NULL) return;
    
    ESP_LOGI(TAG, "Initializing MQTT service...");
    
    // 配置序列化接口
    SerializeInterface motorSerializeInterface = {
        MotorDomainSerialize,
        MotorDomainReserialize
    };
    mqttSerializeArray[PROTO_MOTOR] = motorSerializeInterface;
    
    // 创建 Protocol 层 - JsonProto（JSON协议）
    service->proto = NewJsonProto(mqttComm, mqttSerializeArray, NUM_OF_PROTO);
    if (service->proto == NULL) {
        ESP_LOGE(TAG, "Failed to create json proto");
        return;
    }

    // 发送 hello 测试
    MotorDomain data = {
        .protocol = PROTO_MOTOR,
        .numAngel = 100,
        .denAngel = 180
    };
    ProtoSendPackage(service->proto, (char*)&data, sizeof(MotorDomain));

    // 创建并启动 TaskQue
    g_mqttTaskQue = NewTaskQue(100);
    if (g_mqttTaskQue == NULL) {
        ESP_LOGE(TAG, "Failed to create MQTT task queue");
        return;
    }
    TaskQueStart(g_mqttTaskQue, -1);
    
    // 创建 Router，注入 TaskQue
    service->router = NewRouter(g_mqttTaskQue);
    if (service->router != NULL) {
        RouterHandlerPkg healthHandler = {HealthCommHandler, service};
        RouterRegister(service->router, Health, healthHandler);
        RouterHandlerPkg motorHandler = {MotorHandler, service};
        RouterRegister(service->router, PROTO_MOTOR, motorHandler);
        RouterHandlerPkg errHandler = {ServiceErrHandler, service};
        RouterSetErrHandler(service->router, errHandler);
    }
    
    ServiceStart(service);
    ESP_LOGI(TAG, "MQTT service started");
}

// ==================== 全局初始化入口 ====================

void GlobalInit(void) {
    ESP_LOGI(TAG, "Starting global initialization...");

    // 0. 初始化 NVS（WiFi 需要）
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed");
    }

    // 1. 初始化 GPIO 引脚
    //GpioInit();

    // 2. 初始化串口
    InitSerials();
    
    // 3. 初始化 WiFi
    GlobalWifiInit();
    
    // 4. 初始化 MQTT
    MqttInit();
    
    // 5. 创建并初始化 UART 服务（SerialProto + UART_NUM_2）
    g_uartService = NewService();
    if (g_uartService != NULL) {
        UartServiceInit(g_uartService);
    }
    
    // 6. 创建并初始化 MQTT 服务（JsonProto + mqtt_comm）
    g_mqttService = NewService();
    if (g_mqttService != NULL) {
        MqttServiceInit(g_mqttService);
    }

    ESP_LOGI(TAG, "Global initialization completed");
}

// ==================== 获取 GPIO 映射 ====================

const GpioPinMap* GetGpioPinMap(void) {
    return &g_gpio_map;
}
