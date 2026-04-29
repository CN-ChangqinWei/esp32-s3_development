#ifndef _PLATFORM_H
#define _PLATFORM_H
#include<stdio.h>



extern void* (*robotMalloc)(size_t size);
extern void  (*robotFree)(void* p);


#endif