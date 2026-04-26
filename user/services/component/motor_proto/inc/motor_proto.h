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
MotorInterface MotorProtoInterfaces();
void InitMotorProto(Motor* motor,Proto* proto,int id);
#endif