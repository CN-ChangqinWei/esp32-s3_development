#ifndef _COMMUNICATION_H
#define _COMMUNICATION_H

#include "freertos/FreeRTOS.h"
#include <stdint.h>

// 通信接口抽象 - 只保留基础 send/recv
typedef struct {
    uint32_t (*send)(void* instance, uint8_t* buf, uint32_t len);
    uint32_t (*recv)(void* instance, uint8_t* buf, uint32_t len);
} CommInterface;

// Communication 实例 - 简化，只保留实例和接口
typedef struct {
    CommInterface interface;
    void* instance;
} Communication;

// 创建/销毁
Communication* NewCommunication(void* instance, CommInterface interface);
void DeleteCommunication(Communication* comm);

// 基础操作 - 直接透传到底层
uint32_t CommSend(Communication* comm, uint8_t* buf, uint32_t len);
uint32_t CommRecv(Communication* comm, uint8_t* buf, uint32_t len);

#endif
