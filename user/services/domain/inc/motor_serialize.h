#ifndef _MOTOR_SERIALIZE_H
#define _MOTOR_SERIALIZE_H
#include"motor_domain.h"
#include"cJSON.h"
void* MotorDomainReserialize(char* jsonStr);
char* MotorDomainSerialize(void* domain);


#endif
