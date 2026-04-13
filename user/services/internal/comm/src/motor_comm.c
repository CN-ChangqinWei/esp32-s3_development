
#include"service.h"
#include"motor_service.h"
#include"motor_domain.h"
uint16_t MotorHandler(void*instance,void* arg){
    Service* service = (Service*)instance;
    int res=MotorExec(service,arg);
    MotorDomainReply re={PROTO_MOTOR,res};
    ServiceComm(service,(char*)&re,sizeof(MotorDomainReply));
    
    return 0;
}