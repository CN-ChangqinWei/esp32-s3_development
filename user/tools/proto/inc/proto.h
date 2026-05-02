#ifndef _PROTO_H
#define _PROTO_H

#include "freertos/FreeRTOS.h"

typedef struct {
    void* (*recvPackage)(void* instance, int* len);
    int   (*sendPackage)(void* instance, char* buf, int len);
    void  (*deleteProto)(void* instance);
    int   (*sendBranchPackages)(void*instance,char** bufs,int num);//一次性封装并且发送多个数据包,bufs是每个数据包的指针，num是数据包的数量
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
