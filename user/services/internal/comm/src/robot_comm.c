#include "robot_comm.h"
#include "service.h"
#include "robot_service.h"
#include "robot_domain.h"
#include "proto.h"

uint16_t RobotHandler(void* instance, void* arg) {
    Service* service = (Service*)instance;
    
    // ① 调用业务层
    RobotResult res = RobotExec(
        service->services[PROTO_ROBOT_POSITION], arg);
    
    // ② 构造响应
    RobotDomainReply reply = {PROTO_ROBOT_POSITION, res};
    
    // ③ 发送响应
    if (service != NULL && service->proto != NULL) {
        ProtoSendPackage(service->proto, (char*)&reply, sizeof(RobotDomainReply));
    }
    return 0;
}
