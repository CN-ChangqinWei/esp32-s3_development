#ifndef _MOTOR_REPO_H
#define _MOTOR_REPO_H

#include"motor_service.h"
#include"motor.h"
typedef struct{
    Motor* motors;
    int     len;
    MotorRepoInterface interface;
}MotorRepo;

MotorRepo* NewMotorReop(Motor* motors,int len);
//
char isMotorExsits(void*repo,int id);
char setPosition(void*repo,int id,uint32_t numAngel,uint32_t denAngel,uint32_t maxAngel); 
char setPositionByEncode(void* repo,int id,int encode);
char setPwm(void*repo,int id,int pwmNum,int pwmDen);
char setSpeedByAngel(void* repo,int id,int spNumAngel,int spDenAngel);
char powerOn(void* repo,int id);
char shutOff(void* repo,int id);
#endif
