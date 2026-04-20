#ifndef _MQTT_COMM_H
#define _MQTT_COMM_H
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "communication.h"
#include "mqtt_client.h"

// MQTT 配置结构体
typedef struct {
    char uri[128];          // MQTT 服务器地址，如 "mqtt://192.168.1.100:1883"
    char client_id[32];     // 客户端 ID
    char username[32];      // 用户名（可选）
    char password[32];      // 密码（可选）
    char pub_topic[64];     // 默认发布主题
    char sub_topic[64];     // 默认订阅主题
    int qos;                // QoS 等级 (0, 1, 2)
    int keepalive;          // 保活时间（秒）
} MqttConfig;

// 接收数据节点（用于链表缓冲区）
typedef struct MqttRecvNode {
    char* data;
    uint32_t len;
    struct MqttRecvNode* next;
} MqttRecvNode;

// MqttComm 结构体
typedef struct {
    esp_mqtt_client_handle_t client;    // ESP32 MQTT 客户端句柄
    MqttConfig config;                   // 配置副本
    int connected;                       // 连接状态
    
    // 接收数据缓冲区（链表，支持多条消息）
    MqttRecvNode* recvHead;
    MqttRecvNode* recvTail;
    uint32_t recvTotalLen;               // 当前缓冲的总数据长度
    SemaphoreHandle_t recvMutex;         // 保护接收缓冲区
    
    // 临时接收指针（用于单次 recv 调用跨多次调用）
    MqttRecvNode* curRecvNode;
    uint32_t curRecvOffset;
} MqttComm;

// 创建 MqttComm 对象
Communication* NewMqttComm(MqttConfig* config);

// 销毁 MqttComm 对象
void DeleteMqttComm(void* mqttComm);

// 适配 Communication 接口的发送函数
uint32_t MqttCommSend(void* instance, char* data, uint32_t len);

// 适配 Communication 接口的接收函数
uint32_t MqttCommRecv(void* instance, char* data, uint32_t len);

// 获取 MqttComm 对应的 CommInterface
CommInterface GetMqttCommInterface(void);

// 便捷函数：直接使用 MqttComm 创建 Communication 对象
Communication* NewCommunicationFromMqttComm(MqttComm* instance);

// 检查 MQTT 是否已连接
int MqttCommIsConnected(void* instance);

// 订阅主题
int MqttCommSubscribe(void* instance, const char* topic, int qos);

// 发布到指定主题
int MqttCommPublish(void* instance, const char* topic, char* data, uint32_t len, int qos);

#endif
