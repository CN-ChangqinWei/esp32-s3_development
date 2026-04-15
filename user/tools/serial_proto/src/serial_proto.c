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

void DeleteSerialProto(SerialProto* proto) {
    if (proto != NULL) {
        if (proto->packageBuf != NULL) {
            vPortFree(proto->packageBuf);
        }
        vPortFree(proto);
    }
}

uint8_t SerialProtoInit(SerialProto* proto, Communication* comm) {
    if (proto == NULL) return 1;
    proto->comm = comm;
    proto->state = PROTO_STATE_LEN;
    proto->totalLen = 0;
    proto->remainLen = 0;
    proto->packageBuf = NULL;
    return 0;
}

void* SerialProtoRecvPackage(SerialProto* proto, int* len) {
    if (proto == NULL || proto->comm == NULL) return NULL;
    if (len != NULL) *len = 0;

    if (proto->state == PROTO_STATE_LEN) {
        // 接收4字节长度
        uint32_t tempLen = 0;
        uint32_t recvBytes = CommRecv(proto->comm, (uint8_t*)&tempLen, sizeof(uint32_t));
        
        if (recvBytes < sizeof(uint32_t)) {
            return NULL; // 长度未接收完整
        }

        proto->totalLen = tempLen;
        proto->remainLen = tempLen;

        // 分配缓冲区
        proto->packageBuf = (uint8_t*)pvPortMalloc(proto->totalLen);
        if (proto->packageBuf == NULL) {
            proto->totalLen = 0;
            proto->remainLen = 0;
            return NULL;
        }

        proto->state = PROTO_STATE_DATA;
        return NULL; // 长度接收完成，等待数据

    } else if (proto->state == PROTO_STATE_DATA) {
        // 接收数据到缓冲区当前偏移位置
        uint8_t* writePos = proto->packageBuf + (proto->totalLen - proto->remainLen);
        uint32_t recvBytes = CommRecv(proto->comm, writePos, proto->remainLen);
        proto->remainLen -= recvBytes;

        // 接收完成
        if (proto->remainLen == 0) {
            uint8_t* result = proto->packageBuf;
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

void SerialProtoSendPackage(SerialProto* proto, uint8_t* data, int len) {
    if (proto == NULL || proto->comm == NULL || data == NULL || len <= 0) {
        return;
    }

    // 先发长度（4字节）
    uint32_t tempLen = (uint32_t)len;
    CommSend(proto->comm, (uint8_t*)&tempLen, sizeof(uint32_t));

    // 再发数据
    CommSend(proto->comm, data, len);
}
