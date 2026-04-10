
#ifndef _SERVICE_H
#define _SERVICE_H
#include"serial_comm.h"
#include"router.h"
#include"freertos/FreeRTOS.h"
#include"communication.h"
#include"string.h"
#include"protocol.h"
typedef struct{
    Communication* listener;
}Service;

void SerivceInit();
void ServiceExec();
void ServiceCommHanlder();
void ServiceComm(char* buf,int len);
uint16_t ServiceErrHandler(void*instance,void* arg);
#endif
