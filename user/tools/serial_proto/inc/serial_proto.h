#ifndef _SERIAL_PROTO_H
#define _SERIAL_PROTO_H

#include "freertos/FreeRTOS.h"
#include <stdint.h>
#include "communication.h"
#include "proto.h"

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
    char* packageBuf;          // 包缓冲区
} SerialProto;

// 创建/销毁
SerialProto* NewSerialProto(Communication* comm);
void DeleteSerialProto(void* p);

// 初始化
char SerialProtoInit(SerialProto* proto, Communication* comm);

// 包操作（与 ProtoInterface 匹配的签名）
void* SerialProtoRecvPackage(void* p, int* len);
int SerialProtoSendPackage(void* p, char* data, int len);

// 批量发送多个数据包
int SerialProtoSendBranchPackages(void* p, char** bufs, int num);

// 获取接口
ProtoInterface SerialProtoInterface(void);

#endif
