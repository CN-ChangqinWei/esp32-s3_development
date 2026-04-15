#include "service.h"
#include "serial_proto.h"
#include "router.h"
#include "serial.h"
#include "freertos/FreeRTOS.h"
#include "string.h"
Service* NewService() {
    Service* service = pvPortMalloc(sizeof(Service));
    if (service != NULL) {
        memset(service, 0, sizeof(Service));
    }
    return service;
}

void DeleteService(Service* service) {
    if (service != NULL) {
        if (service->router != NULL) {
            DeleteRouter(service->router);
        }
        if (service->proto != NULL) {
            DeleteSerialProto(service->proto);
        }
        vPortFree(service);
    }
}

void ServiceExec(Service* service) {
    if (service == NULL || service->router == NULL) return;
    RouterExec(service->router);
}

void ServiceComm(Service* service, char* buf, int len) {
    if (service == NULL || service->proto == NULL) return;
    SerialProtoSendPackage(service->proto, (uint8_t*)buf, len);
}

uint16_t ServiceErrHandler(void* instance, void* arg) {
    Service* service = (Service*)instance;
    if (service == NULL || service->proto == NULL) return 1;
    char msg[60] = {0};
    sprintf(msg, "errhandler:%s", (char*)arg);
    SerialProtoSendPackage(service->proto, (uint8_t*)msg, strlen(msg));
    return 0;
}
