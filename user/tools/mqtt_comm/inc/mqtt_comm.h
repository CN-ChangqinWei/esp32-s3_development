#ifndef _MQTT_COMM_H
#define _MQTT_COMM_H
#include "freertos/FreeRTOS.h"
#include "communication.h"
#include "mqtt_client.h"

// MqttComm 结构体，继承/包装 Mqtt
typedef struct{

}MqttConfig;

typedef struct {
    
    MqttConfig config;
   
} MqttComm;

// 创建 MqttComm 对象
Communication* NewMqttComm(MqttConfig config);

// 销毁 MqttComm 对象
void DeleteMqttComm(void* MqttComm);

// 适配 Communication 接口的发送函数
uint32_t MqttCommSend(void* instance, char* data, uint32_t len);

// 适配 Communication 接口的接收函数
uint32_t MqttCommRecv(void* instance, char* data, uint32_t len);

// 获取 MqttComm 对应的 CommInterface
CommInterface GetMqttCommInterface(void);

// 便捷函数：直接使用 Mqtt 创建 Communication 对象


#endif
