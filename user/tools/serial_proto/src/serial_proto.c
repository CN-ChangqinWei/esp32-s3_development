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

    // 先发长度（4字节）
    uint32_t tempLen = (uint32_t)len;
    CommSend(proto->comm, (char*)&tempLen, sizeof(uint32_t));

    // 再发数据
    return CommSend(proto->comm, data, len);
}
ProtoInterface SerialProtoInterface(){
    ProtoInterface interfaces={
        SerialProtoRecvPackage,
        SerialProtoSendPackage,
        DeleteSerialProto
    };
    return interfaces;
}