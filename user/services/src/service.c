#include "service.h"
#include "serial_proto.h"
#include "router.h"
#include "serial.h"
#include "freertos/FreeRTOS.h"
#include "string.h"

void* (*serviceMalloc)(size_t size)=pvPortMalloc;
void (*serviceFree)(void* p)=vPortFree;



Service* NewService(int srvLen) {
    Service* service = serviceMalloc(sizeof(Service));
    if (service != NULL) {
        memset(service, 0, sizeof(Service));
    }
    service->srvLen=srvLen;
    service->services=serviceMalloc(sizeof(void*)*srvLen);
    memset(service->services,0,srvLen*sizeof(void*));
    return service;
}

void DeleteService(Service* service) {
    if (service != NULL) {
        if (service->router != NULL) {
            DeleteRouter(service->router);
        }
        if (service->proto != NULL) {
            DeleteProto(service->proto);
        }
        serviceFree(service);
    }
}

void ServiceExec(Service* service) {
    // if (service == NULL || service->router == NULL) return;
    // RouterExec(service->router);
}

void ServiceComm(Service* service, char* buf, int len) {
    if (service == NULL || service->proto == NULL) return;
    ProtoSendPackage(service->proto, buf, len);
}

uint16_t ServiceErrHandler(void* instance, void* arg) {
    // Service* service = (Service*)instance;
    // if (service == NULL || service->proto == NULL) return 1;
    // char msg[60] = {0};
    // sprintf(msg, "errhandler:%s", (char*)arg);
    // ProtoSendPackage(service->proto, msg, strlen(msg));
    return 0;
}

static void ServiceCommHanlder(void* p) {
    Service* srv = (Service*)p;
    while (1) {
        if (srv->proto == NULL) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        int len = 0;
        void* buf = ProtoRecvPackage(srv->proto, &len);

        if (buf != NULL) {
            RouterAnlyPackage(srv->router, buf, len);
        }
        vTaskDelay(pdMS_TO_TICKS(10));  // 10ms 延时，避免CPU占用过高
    }
}

void ServiceStart(Service* service){
    // 增加栈大小到 4096，避免在处理复杂业务（如逆运动学解算）时栈溢出
    xTaskCreate(ServiceCommHanlder, "service_comm_handler", 4096,
                service, 4, &service->handler);
}

void ServiceRegister(Service* service, Protocol protocol, void* instance, RouterHandler handler){
    if (service == NULL) return;
    
    // 1. 将业务实例指针存入 services 数组（以 protocol 为索引）
    if (service->services != NULL && protocol < service->srvLen) {
        (service->services)[protocol] = instance;
    }
    
    // 2. 注册路由 handler（包含 service 自身指针和 handler 函数）
    if (service->router != NULL) {
        RouterHandlerPkg pkg = {
            .handler = handler,
            .instance = service  // service 自身作为 instance 传递给 handler
        };
        RouterRegister(service->router, protocol, pkg);
    }
}
