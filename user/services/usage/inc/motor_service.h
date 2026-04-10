
#ifndef _MOTOR_SERVICE_H
#define _MOTOR_SERVICE_H
#include "stdio.h"
#include <stdint.h>
#include "motor_domain.h"
#include "freertos/FreeRTOS.h"
typedef struct {
    char (*isMotorExsits)(void*repo,int id);
    char (*setPosition)(void*repo,int id,uint32_t numAngel,uint32_t denAngel,uint32_t maxAngel); 
    char (*setPositionByEncode)(void* repo,int id,int encode);
    char (*setPwm)(void*repo,int id,int pwmNum,int pwmDen);
    char (*setSpeedByAngel)(void* repo,int id,int spNumAngel,int spDenAngel);
    char (*powerOn)(void* repo,int id);
    char (*shutOff)(void* repo,int id);
}MotorRepoInterface;

typedef struct{
    void* repo;
    MotorRepoInterface interface;
}MotorService;

int MotorExec(void* service,void* arg);//业务层执行，
MotorService* NewMotorService(void* repo,MotorRepoInterface interface);
#endif
