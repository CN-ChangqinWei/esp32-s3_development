#ifndef _MULTI_MOTOR_DOMAIN
#define _MULTI_MOTOR_DOMAIN
#include"motor_domain.h"

typedef struct{
    int protocol;
    int nums;//有几个motordomain
}MultiMotorDomain;

typedef struct{
    int protocol;
    MotorResult res;
}MultiMotorDomainReply;

#endif