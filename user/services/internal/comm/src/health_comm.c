#include "health_comm.h"
#include "health_service.h"
#include "service.h"
#include "serial_proto.h"

uint16_t HealthCommHandler(void* instance, void* arg) {
    Service* service = (Service*)instance;
    int len;
    char* res = HealthExec(&len);
    if (service != NULL && service->proto != NULL) {
        SerialProtoSendPackage(service->proto, (char*)res, len);
    }
    return 0;
}
