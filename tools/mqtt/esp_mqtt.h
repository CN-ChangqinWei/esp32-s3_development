
#ifndef _ESP_MQTT_H
#define _ESP_MQTT_H
#include"mqtt_client.h"
#include"ring_buf.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#define _MQTT_MALLOC(size)    (pvPortMalloc(size))
typedef struct{
    char url[50];
    char cliendId[50];
    char username[50];
    char pswd[50];
}MQTTConfig;
typedef struct{//根据mqtt库的形式添加成员
    RingBuf* recvBuf;
    MQTTConfig config;
    esp_mqtt_client_handle_t* client;
}MQTT;

int   MqttRecv(MQTT* mqtt,char* buf,int len);//读取recvBuf最长len的数据到buf,返回值为接受的字节数
int   MqttSend(MQTT* mqtt,char* buf,int len);//发送len的buf数据,返回值为发送的字节数
void  MqttDelete(MQTT** mqtt);
MQTT* NewMqtt(MQTTConfig config,int bufSize);//用_MQTT_MALLOC宏来创建对象返回指针,创建的时候自动连接URL并且自动订阅名称为username的topic,并且接收数据的钩子函数为把数据添加到recvBuf中
int   MqttDisconnect(MQTT* mqtt);
int   MqttConnect(MQTT* mqtt);

#endif
