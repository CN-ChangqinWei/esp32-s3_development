#ifndef _MOTOR_H
#define _MOTOR_H
#include <stdint.h>
#include <stdio.h>
extern void* (*motorMalloc)(size_t size);
extern void  (*motorFree)(void* p);
typedef struct{
    int (*powerOn)(void*instance);
    int (*shutDown)(void*instance);
    int (*setPosition)(void*instance,int numAngel,int denAngel,int maxAngel);
    int (*setPositionByEncode)(void*instance,int encode);
    int (*setSpeedByPwm)(void*instance,int pwmNum,int pwmDen);
    int (*setSpeedByAngel)(void*instance,int spNumAngel,int spDenAngel);
    int (*setSpeedByEncode)(void*instance,int encode);
}MotorInterface;
typedef struct {
    void*       instance;
    MotorInterface interface;

}Motor;
Motor* NewMotor(void*instance,MotorInterface interface);
#endif
