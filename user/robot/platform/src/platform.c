#include"platform.h"
#include"freertos/FreeRTOS.h"
#define _ROBOT_OS_FREERTOS

//#define _ROBOT_OS_DEFAULT 
//#define _ROBOT_OS_LINUX
//#define _ROBOT_OS_WINDOWS
#ifdef _ROBOT_OS_DEFAULT
void* (*robotMalloc)(size_t size)=malloc;
void  (*robotFree)(void* p)=free;
#elif defined(_ROBOT_OS_FREERTOS)
void* (*robotMalloc)(size_t size)=pvPortMalloc;
void  (*robotFree)(void* p)=vPortFree;
#endif
