#ifndef _COMMUNICATION_H
#define _COMMUNICATION_H
#include"freertos/FreeRTOS.h"
#include <stdint.h>
typedef enum{
    MODE_LEN=0,
    MODE_DATA
}CommStatu;
typedef struct{
    uint32_t (*send)(void*,uint8_t*,uint32_t);//输入参数分别是实例，缓冲区，长度
    uint32_t (*recv)(void*,uint8_t*,uint32_t);
}CommInterface;
typedef struct{
    CommInterface interface;
    void* instance;
    uint16_t statu;//接收模式
    uint32_t recvLen;//接收DATA模式下还需要接收的长度
    uint32_t totalLen;
    uint8_t* packageBuf;
    uint32_t bufCur;
}Communication;
Communication* NewCommunication(void* instance,CommInterface interface);
void DeleteCommunication(Communication* comm);
void * CommRecvPackage(Communication* comm,int*len);
void   CommSendPackage(Communication* comm,uint8_t* data,int len);
void   CommHandler(Communication* comm);

#endif
