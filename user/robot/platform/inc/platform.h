#ifndef _PLATFORM_H
#define _PLATFORM_H
#include<stdio.h>

//axis 的精度
typedef double AxisFloat;
//

#define _FUNC_RANGE(value,left,right) ((value)<(left)?(left):((value)>(right)?(right):(value)))

extern void* (*robotMalloc)(size_t size);
extern void  (*robotFree)(void* p);


#endif