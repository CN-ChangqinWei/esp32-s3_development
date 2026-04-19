
#include "service.h"
#include "motor_service.h"
#include "motor_domain.h"
#include "serial_proto.h"

uint16_t MotorHandler(void* instance, void* arg) {
    Service* service = (Service*)instance;
    int res = MotorExec(service, arg);
    MotorDomainReply re = {PROTO_MOTOR, res};
    if (service != NULL && service->proto != NULL) {
        SerialProtoSendPackage(service->proto, (char*)&re, sizeof(MotorDomainReply));
    }
    return 0;
}