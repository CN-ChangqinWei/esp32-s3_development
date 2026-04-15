#ifndef _SERVICE_H
#define _SERVICE_H

#include "serial_comm.h"
#include "router.h"
#include "freertos/FreeRTOS.h"
#include "serial_proto.h"
#include "protocol.h"

typedef struct {
    SerialProto* proto;        // 协议层实例（替代 communication）
    TaskHandle_t handler;
    Router* router;
} Service;

Service* NewService();
void DeleteService(Service* service);
void ServiceExec(Service* service);

void ServiceComm(Service* service, char* buf, int len);
uint16_t ServiceErrHandler(void* instance, void* arg);

#endif
