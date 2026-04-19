#include "proto.h"
#include <string.h>

static void* (*protoMalloc)(size_t size) = pvPortMalloc;
static void (*protoFree)(void*) = vPortFree;

Proto* NewProto(void* instance, ProtoInterface interfaces) {
    Proto* proto = protoMalloc(sizeof(Proto));
    if (NULL == proto) return NULL;
    memset(proto, 0, sizeof(Proto));
    proto->instance = instance;
    proto->interfaces = interfaces;
    return proto;
}

void* ProtoRecvPackage(void* p, int* len) {
    Proto* proto = p;
    if (proto == NULL || proto->instance == NULL) return NULL;
    return proto->interfaces.recvPackage(proto->instance, len);
}

int ProtoSendPackage(void* p, char* buf, int len) {
    Proto* proto = p;
    if (proto == NULL || proto->instance == NULL || buf == NULL || len <= 0) return 0;
    return proto->interfaces.sendPackage(proto->instance, buf, len);
}

void DeleteProto(void* p) {
    Proto* proto = p;
    if (proto == NULL) return;
    if (proto->interfaces.deleteProto != NULL && proto->instance != NULL) {
        proto->interfaces.deleteProto(proto->instance);
    }
    protoFree(proto);
}
