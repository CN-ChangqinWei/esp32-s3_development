#ifndef _SERIAL_PROTO_H
#define _SERIAL_PROTO_H

#include "freertos/FreeRTOS.h"
#include <stdint.h>
#include "communication.h"

// 协议接收状态
typedef enum {
    PROTO_STATE_LEN = 0,   // 等待接收4字节长度
    PROTO_STATE_DATA       // 等待接收数据
} ProtoState;

// SerialProto 实例
typedef struct {
    Communication* comm;       // 底层通信实例
    ProtoState state;          // 当前接收状态
    uint32_t totalLen;         // 总数据长度
    uint32_t remainLen;        // 剩余待接收长度
    uint8_t* packageBuf;       // 包缓冲区
} SerialProto;

// 创建/销毁
SerialProto* NewSerialProto(Communication* comm);
void DeleteSerialProto(SerialProto* proto);

// 初始化
uint8_t SerialProtoInit(SerialProto* proto, Communication* comm);

// 包操作
// 接收完整包（非阻塞，返回NULL表示未收完）
void* SerialProtoRecvPackage(SerialProto* proto, int* len);

// 发送包（先发4字节长度，再发数据）
void SerialProtoSendPackage(SerialProto* proto, uint8_t* data, int len);

#endif
