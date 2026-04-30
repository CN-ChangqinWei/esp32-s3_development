#ifndef _PLATFORM_H
#define _PLATFORM_H
#include<stdio.h>

//axis 的精度
typedef double AxisFloat;
//

extern void* (*robotMalloc)(size_t size);
extern void  (*robotFree)(void* p);


#endif