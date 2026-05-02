#ifndef _JSON_PROTO_H
#define _JSON_PROTO_H

#include "communication.h"
#include "proto.h"

// 序列化接口
typedef char* (*JsonSerialize)(void* p);
typedef void* (*JsonReserialize)(char* p);

typedef struct {
    JsonSerialize serialize;
    JsonReserialize reserialize;
} SerializeInterface;

// JsonProto 结构体
typedef struct {
    Communication* comm;           // 底层通信实例
    SerializeInterface* interfacesArray;  // 协议序列化接口数组
    int interfacesLen;             // 接口数组长度
    
    // JSON 接收相关
    char* recvBuf;                 // 接收缓冲区
    int recvBufSize;               // 缓冲区大小
    int recvLen;                   // 当前接收长度
    int braceCount;                // 大括号计数器
    int inJson;                    // 是否在 JSON 中
    int jsonStart;                 // JSON 起始位置
} JsonProto;

// 创建 JsonProto 实例
Proto* NewJsonProto(Communication* comm, SerializeInterface* interfacesArray, int len);

// 删除 JsonProto 实例
void DeleteJsonProto(void* p);

// 接收 JSON 包（完整大括号匹配后提取 protocol，调用对应反序列化函数）
void* JsonProtoProtoRecvPackage(void* p, int* len);

// 发送 JSON 包（根据前4字节协议值，使用对应序列化函数）
int JsonProtoSendPackage(void* p, char* data, int len);

// 批量发送多个 JSON 数据包
int JsonProtoSendBranchPackages(void* p, char** bufs, int num);

// 获取 ProtoInterface
ProtoInterface JsonProtoInterface(void);

#endif
