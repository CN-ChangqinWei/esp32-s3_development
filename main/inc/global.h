#ifndef _GLOBAL_H
#define _GLOBAL_H

#include "serial.h"
#include "serial_comm.h"
#include "service.h"

// Serial 全局实例
extern Serial* serial1;
extern uint8_t sendBuf1[255];

// SerialComm 全局实例
extern SerialComm* serialComm;

// Service 全局实例
extern Service* g_service;

// 全局初始化函数
void GlobalInit(void);

#endif
