#include "health_comm.h"
#include "health_service.h"
#include "service.h"
#include "proto.h"

uint16_t HealthCommHandler(void* instance, void* arg) {
    (void)arg;  // 未使用参数
    Service* service = (Service*)instance;
    int len;
    char* res = HealthExec(&len);
    if (service != NULL && service->proto != NULL && res != NULL && len > 0) {
        ProtoSendPackage(service->proto, res, len);
    }
    return 0;
}
