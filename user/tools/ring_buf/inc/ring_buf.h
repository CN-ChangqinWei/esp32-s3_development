#ifndef _RING_BUF_H
#define _RING_BUF_H
#include"freertos/FreeRTOS.h"
#include <stdint.h>

typedef struct{
    char* buffer;
    int size;
    int head;
    int tail;
    int len;
}RingBuf;
RingBuf NewRingBuf(int size);
RingBuf NewRingBufByOther(int size,char* buf);
uint8_t RingBufAddByte(RingBuf*ring,char byte);//尾部添加一个字节,长度不够那么直接弹出头部，返回值为是否插入成功
uint8_t RingBufAddData(RingBuf*ring,char* data,int len);//尾部添加指定区域数据,长度不够那么直接弹出头部，返回值为是否插入成功
char    RingBufPop(RingBuf*ring);//弹出头部字节
uint32_t RingBufRead(RingBuf*ring,char* buf,int len);//弹出头部多个数据并且复制到指定buf


#endif