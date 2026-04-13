#include "global.h"
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "communication.h"
#include "router.h"
#include "health_comm.h"
#include "motor_comm.h"
#include "service.h"

static const char* TAG = "GLOBAL";

// Serial 全局实例定义
Serial* serial1 = NULL;
uint8_t sendBuf1[_SERIAL_BUF_SIZE] = {0};

// SerialComm 全局实例定义
SerialComm* serialComm = NULL;

// Service 全局实例定义
Service* g_service = NULL;

static void InitSerials(void) {
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

    // 启动接收任务
    SerialStartRxTask(serial1, 5);
    ESP_LOGI(TAG, "Serial1 initialized successfully");
}

static void ServiceCommHanlder(void* p){
    Service* srv = (Service*)p;
    if(srv->listener == NULL) return;
    int len=0;
    char* buf=CommRecvPackage(srv->listener,&len);
    if(NULL==buf) return;
    RouterAnlyPackage(srv->router,buf,len);
}

static void ServiceInit(Service* service){
    if(service == NULL) return;
    
    service->listener = NewCommunicationFromSerial(serialComm);
    if(service->listener != NULL){
        CommSendPackage(service->listener,(uint8_t*)"hello",strlen("hello"));
    }
    service->router = NewRouter();
    if(service->router != NULL){
        RouterInit(service->router);
        RouterHandlerPkg healthHandler = {HealthCommHandler,service};
        RouterRegister(service->router,Health, healthHandler);
        RouterHandlerPkg motorHandler = {MotorHandler,service};
        RouterRegister(service->router,PROTO_MOTOR, motorHandler);
        RouterHandlerPkg errHandler ={ServiceErrHandler,service};
        RouterSetErrHandler(service->router,errHandler);
    }
    xTaskCreate(ServiceCommHanlder, "service_comm_handler", 2048,
                              service, 4, &service->handler);
}

void GlobalInit(void) {
    ESP_LOGI(TAG, "Starting global initialization...");

    // 1. 初始化串口
    InitSerials();

    // 2. 创建 SerialComm
    if (serial1 != NULL) {
        serialComm = NewSerialComm(serial1);
    }

    // 3. 创建并初始化 Service
    g_service = NewService();
    if (g_service != NULL) {
        ServiceInit(g_service);
    }

    ESP_LOGI(TAG, "Global initialization completed");
}
