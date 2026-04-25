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
int isMotorExsits(void*repo,int id);
int setPosition(void*repo,int id,int numAngel,int denAngel,int maxAngel); 
int setPositionByEncode(void* repo,int id,int encode);
int setPwm(void*repo,int id,int pwmNum,int pwmDen);
int setSpeedByAngel(void* repo,int id,int spNumAngel,int spDenAngel);
int powerOn(void* repo,int id);
int shutOff(void* repo,int id);
#endif
