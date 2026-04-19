#include"proto.h"
static void* (*protoMalloc)(size_t size) = pvPortMalloc;
static void (*protoFree)(void*) = vPortFree;
Proto* NewProto(void* instance,ProtoInterface interfaces){
    Proto* proto = protoMalloc(sizeof(Proto));
    if(NULL==proto) return NULL;
    proto->instance=instance;
    proto->interfaces=interfaces;
    return proto;
}
void* ProtoRecvPackage(void* instance,int* len){
    Proto* proto = instance;
    return proto->interfaces.recvPackage(instance,len);
}
int   ProtoSendPackage(void* instance,char* buf,int len){
    Proto* proto = instance;
    return proto->interfaces.sendPackage(instance,buf,len);
}

void DeleteProto(void* p){
    Proto* proto = p;
    proto->interfaces.deleteProto(proto->instance);
    protoFree(proto);
}
