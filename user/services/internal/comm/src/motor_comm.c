
#include"service.h"
#include"motor_service.h"
#include"motor_domain.h"
uint16_t MotorHandler(void*instance,void* arg){
    int res=MotorExec(instance,arg);
    MotorDomainReply re={PROTO_MOTOR,res};
    ServiceComm((char*)&re,sizeof(MotorDomainReply));
    
    return 0;
}