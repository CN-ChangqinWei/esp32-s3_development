#ifndef _MOTOR_H
#define _MOTOR_H
#include <stdint.h>
#include <stdio.h>
typedef struct{
    uint8_t (*powerOn)(void*instance);
    uint8_t (*shutDown)(void*instance);
    uint8_t (*setPosition)(void*instance,uint32_t numAngel,uint32_t denAngel,uint32_t maxAngel);
    uint8_t (*setPositionByEncode)(void*instance,int encode);
    uint8_t (*setSpeedByPwm)(void*instance,int pwmNum,int pwmDen);
    uint8_t (*setSpeedByAngel)(void*instance,int spNumAngel,int spDenAngel);
    uint8_t (*setSpeedByEncode)(void*instance,int encode);
}MotorInterface;
typedef struct {
    void*       instance;
    MotorInterface interface;

}Motor;
Motor* NewMotor(void*instance,MotorInterface interface);
#endif
