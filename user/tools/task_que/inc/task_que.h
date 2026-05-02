#ifndef _TASK_QUE_H
#define _TASK_QUE_H

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

typedef uint16_t (*TaskFunc)(void* instance, void* arg);
typedef struct{
    TaskFunc func;
    void* instance;
    void* data;
}TaskPackage;

typedef struct {
    TaskPackage* ringBuf;
    int len;
    int head;
    int tail;
    int isRuning;
    SemaphoreHandle_t mutex;  // 互斥锁，保护环形缓冲区操作
}TaskQue;
TaskQue* NewTaskQue(int len);
int     TaskQueStart(TaskQue* que,int cpu);
void    TaskQueStop(TaskQue* que);
void    DeleteTaskQue(TaskQue* que);
int    TaskQueAdd(TaskQue* que, TaskPackage pkg);

#endif