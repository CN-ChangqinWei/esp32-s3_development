#ifndef _PROTO_H
#define _PROTO_H

#include "freertos/FreeRTOS.h"

typedef struct {
    void* (*recvPackage)(void* instance, int* len);
    int   (*sendPackage)(void* instance, char* buf, int len);
    void  (*deleteProto)(void* instance);
} ProtoInterface;

typedef struct {
    void* instance;
    ProtoInterface interfaces;
} Proto;

Proto* NewProto(void* instance, ProtoInterface interfaces);
void* ProtoRecvPackage(void* p, int* len);
int   ProtoSendPackage(void* p, char* buf, int len);
void DeleteProto(void* p);

#endif
