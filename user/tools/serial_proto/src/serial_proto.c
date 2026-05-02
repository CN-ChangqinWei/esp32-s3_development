#include "serial_proto.h"
#include <string.h>

SerialProto* NewSerialProto(Communication* comm) {
    SerialProto* proto = (SerialProto*)pvPortMalloc(sizeof(SerialProto));
    if (proto == NULL) return NULL;
    if (SerialProtoInit(proto, comm) != 0) {
        vPortFree(proto);
        return NULL;
    }
    return proto;
}

void DeleteSerialProto(void* p) {
    SerialProto* proto=p;
    if (proto != NULL) {
        if (proto->packageBuf != NULL) {
            vPortFree(proto->packageBuf);
        }
        vPortFree(proto);
    }
}

char SerialProtoInit(SerialProto* proto, Communication* comm) {
    if (proto == NULL) return 1;
    proto->comm = comm;
    proto->state = PROTO_STATE_LEN;
    proto->totalLen = 0;
    proto->remainLen = 0;
    proto->packageBuf = NULL;
    return 0;
}

void* SerialProtoRecvPackage(void* p, int* len) {
    SerialProto* proto = p;
    if (proto == NULL || proto->comm == NULL) return NULL;
    if (len != NULL) *len = 0;

    if (proto->state == PROTO_STATE_LEN) {
        // 接收4字节长度
        uint32_t tempLen = 0;
        uint32_t recvBytes = CommRecv(proto->comm, (char*)&tempLen, sizeof(uint32_t));
        
        if (recvBytes < sizeof(uint32_t)) {
            return NULL; // 长度未接收完整
        }

        proto->totalLen = tempLen;
        proto->remainLen = tempLen;

        // 分配缓冲区
        proto->packageBuf = (char*)pvPortMalloc(proto->totalLen);
        if (proto->packageBuf == NULL) {
            proto->totalLen = 0;
            proto->remainLen = 0;
            return NULL;
        }

        proto->state = PROTO_STATE_DATA;
        return NULL; // 长度接收完成，等待数据

    } else if (proto->state == PROTO_STATE_DATA) {
        // 接收数据到缓冲区当前偏移位置
        char* writePos = proto->packageBuf + (proto->totalLen - proto->remainLen);
        uint32_t recvBytes = CommRecv(proto->comm, writePos, proto->remainLen);
        proto->remainLen -= recvBytes;

        // 接收完成
        if (proto->remainLen == 0) {
            char* result = proto->packageBuf;
            if (len != NULL) {
                *len = proto->totalLen;
            }
            // 重置状态
            proto->state = PROTO_STATE_LEN;
            proto->totalLen = 0;
            proto->remainLen = 0;
            proto->packageBuf = NULL;
            return result;
        }
    }

    return NULL;
}

int SerialProtoSendPackage(void* p, char* data, int len) {
    SerialProto* proto = p;
    if (proto == NULL || proto->comm == NULL || data == NULL || len <= 0) {
        return 0;
    }

    // 计算总大小：4字节长度 + 数据
    int totalLen = sizeof(uint32_t) + len;
    
    // 分配临时缓冲区
    char* tempBuf = (char*)pvPortMalloc(totalLen);
    if (tempBuf == NULL) {
        return 0;
    }
    
    // 拷贝长度（4字节）
    uint32_t tempLen = (uint32_t)len;
    memcpy(tempBuf, &tempLen, sizeof(uint32_t));
    
    // 拷贝数据
    memcpy(tempBuf + sizeof(uint32_t), data, len);
    
    // 一次性发送
    int sent = CommSend(proto->comm, tempBuf, totalLen);
    
    // 释放临时缓冲区
    vPortFree(tempBuf);
    
    return sent > 0 ? len : 0;
}

// 批量发送多个数据包
int SerialProtoSendBranchPackages(void* p, char** bufs, int num) {
    SerialProto* proto = p;
    if (proto == NULL || proto->comm == NULL || bufs == NULL || num <= 0) {
        return 0;
    }
    
    // 计算总大小
    int totalLen = 0;
    int validCount = 0;
    for (int i = 0; i < num; i++) {
        if (bufs[i] == NULL) continue;
        // 每个包：4字节长度 + 数据
        uint32_t len = *(uint32_t*)bufs[i];
        totalLen += sizeof(uint32_t) + len;
        validCount++;
    }
    
    if (validCount == 0 || totalLen == 0) {
        return 0;
    }
    
    // 分配临时缓冲区
    char* tempBuf = (char*)pvPortMalloc(totalLen);
    if (tempBuf == NULL) {
        return 0;
    }
    
    // 拷贝所有数据到缓冲区
    int offset = 0;
    for (int i = 0; i < num; i++) {
        if (bufs[i] == NULL) continue;
        
        // 获取当前包的长度（前4字节）
        uint32_t len = *(uint32_t*)bufs[i];
        
        // 拷贝长度（4字节）
        memcpy(tempBuf + offset, &len, sizeof(uint32_t));
        offset += sizeof(uint32_t);
        
        // 拷贝数据（长度不包含前4字节的长度字段本身）
        memcpy(tempBuf + offset, bufs[i] + sizeof(uint32_t), len);
        offset += len;
    }
    
    // 一次性发送
    int sent = CommSend(proto->comm, tempBuf, totalLen);
    
    // 释放临时缓冲区
    vPortFree(tempBuf);
    
    return sent > 0 ? totalLen : 0;
}

ProtoInterface SerialProtoInterface(){
    ProtoInterface interfaces={
        SerialProtoRecvPackage,
        SerialProtoSendPackage,
        DeleteSerialProto,
        SerialProtoSendBranchPackages
    };
    return interfaces;
}