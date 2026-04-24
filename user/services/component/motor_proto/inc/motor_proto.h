#ifndef _MOTOR_PROTO_H
#define _MOTOR_PROTO_H
#include "motor.h"
#include "proto.h"
#include "motor_domain.h"
#include "protocol.h"
typedef struct{
    Proto* proto;
    int id;
}MotorProto;

Motor* NewMotorProto(Proto* proto,int id);
int powerOn(void*instance);
int shutDown(void*instance);
int setPosition(void*instance,int numAngel,int denAngel,int maxAngel);
int setPositionByEncode(void*instance,int encode);
int setSpeedByPwm(void*instance,int pwmNum,int pwmDen);
int setSpeedByAngel(void*instance,int spNumAngel,int spDenAngel);
int setSpeedByEncode(void*instance,int encode);
MotorInterface MotorProtoInterfaces();
#endif