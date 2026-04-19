#ifndef _SERIAL_COMM_H
#define _SERIAL_COMM_H
#include "freertos/FreeRTOS.h"
#include "communication.h"
#include "serial.h"

// SerialComm 结构体，继承/包装 Serial
typedef struct {
    Serial* serial;
    // 临时接收缓冲区（用于单次recv调用）
    char* tempRecvBuf;
    uint32_t tempRecvLen;
    uint32_t tempRecvCur;
} SerialComm;

// 创建 SerialComm 对象
SerialComm* NewSerialComm(Serial* serial);

// 销毁 SerialComm 对象
void DeleteSerialComm(SerialComm* serialComm);

// 适配 Communication 接口的发送函数
uint32_t SerialCommSend(void* instance, char* data, uint32_t len);

// 适配 Communication 接口的接收函数
uint32_t SerialCommRecv(void* instance, char* data, uint32_t len);

// 获取 SerialComm 对应的 CommInterface
CommInterface GetSerialCommInterface(void);

// 便捷函数：直接使用 Serial 创建 Communication 对象
Communication* NewCommunicationFromSerial(SerialComm* instance);

#endif
