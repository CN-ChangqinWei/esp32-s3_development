#ifndef _SERVICE_H
#define _SERVICE_H

#include "serial_comm.h"
#include "router.h"
#include "freertos/FreeRTOS.h"
#include "serial_proto.h"
#include "protocol.h"
#include "esp_log.h"
#define _SERVICE_LOG ESP_LOGE

extern void* (*serviceMalloc)(size_t size);
extern void (*serviceFree)(void* p);
typedef struct {
    Proto* proto;        // 协议层实例（替代 communication）
    TaskHandle_t handler;
    Router* router;
    int isStart;
    void **services;//存储服务实例的指针，通过protocol获取对应实例
    int  srvLen;
} Service;

Service* NewService(int srvLen);
void DeleteService(Service* service);
void ServiceExec(Service* service);

void ServiceComm(Service* service, char* buf, int len);
uint16_t ServiceErrHandler(void* instance, void* arg);
void ServiceStart(Service* service);
void ServiceRegister(Service* service,Protocol protocol,void* instance,RouterHandler handler);
#endif
