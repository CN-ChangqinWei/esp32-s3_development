#ifndef _MOTOR_SERIALIZE_H
#define _MOTOR_SERIALIZE_H
#include"motor_domain.h"
#include"cJSON.h"
MotorDomain MotorDomainReserialize(char* jsonStr);
char*       MotorDomainSerialize(MotorDomain* domain);


#endif
