#include "motor.h"
#include "freertos/FreeRTOS.h"
static void* (*motorMalloc)(size_t size) =pvPortMalloc;
static void  (*motorFree)(void* p)=vPortFree;


Motor* NewMotor(void*instance,MotorInterface interface){
    if(NULL==instance) return NULL;
    Motor* res = (Motor*)motorMalloc(sizeof(Motor));

    if(NULL!=res){
        res->instance=instance;
        res->interface=interface;
    }
    return res;
}
