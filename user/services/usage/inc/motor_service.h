
#ifndef _MOTOR_SERVICE_H
#define _MOTOR_SERVICE_H
#include "stdio.h"
#include <stdint.h>
#include "motor_domain.h"
#include "freertos/FreeRTOS.h"
typedef struct {
    int (*isMotorExsits)(void*repo,int id);
    int (*setPosition)(void*repo,int id,int numAngel,int denAngel,int maxAngel); 
    int (*setPositionByEncode)(void* repo,int id,int encode);
    int (*setPwm)(void*repo,int id,int pwmNum,int pwmDen);
    int (*setSpeedByAngel)(void* repo,int id,int spNumAngel,int spDenAngel);
    int (*powerOn)(void* repo,int id);
    int (*shutOff)(void* repo,int id);
}MotorRepoInterface;

typedef struct{
    void* repo;
    MotorRepoInterface interface;
}MotorService;

MotorResult MotorExec(void* service,void* arg);//业务层执行，
MotorService* NewMotorService(void* repo,MotorRepoInterface interface);
#endif
