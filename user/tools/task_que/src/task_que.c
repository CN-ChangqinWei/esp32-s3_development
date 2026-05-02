#include "task_que.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static void* (*queMalloc)(size_t size) = pvPortMalloc;
static void  (*queFree)(void* p) = vPortFree;

TaskQue* NewTaskQue(int len) {
    if (len <= 0) return NULL;
    
    TaskQue* que = (TaskQue*)queMalloc(sizeof(TaskQue));
    if (que == NULL) return NULL;
    
    memset(que, 0, sizeof(TaskQue));
    
    que->ringBuf = (TaskPackage*)queMalloc(sizeof(TaskPackage) * len);
    if (que->ringBuf == NULL) {
        queFree(que);
        return NULL;
    }
    
    memset(que->ringBuf, 0, sizeof(TaskPackage) * len);
    que->len = len;
    que->head = 0;
    que->tail = 0;
    que->isRuning = 0;
    
    // 创建互斥锁
    que->mutex = xSemaphoreCreateMutex();
    if (que->mutex == NULL) {
        queFree(que->ringBuf);
        queFree(que);
        return NULL;
    }
    
    return que;
}

static void TaskHandler(void* arg) {
    TaskQue* que = (TaskQue*)arg;
    
    while (que->isRuning) {
        TaskPackage pkg = {0};
        int hasTask = 0;
        
        // 从队列中获取任务（带锁保护）
        if (xSemaphoreTake(que->mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (que->head != que->tail) {
                pkg = que->ringBuf[que->head];
                que->head = (que->head + 1) % que->len;
                hasTask = 1;
            }
            xSemaphoreGive(que->mutex);
            
            if (hasTask && pkg.func != NULL) {
                // 调用任务函数: func(instance, data)
                pkg.func(pkg.instance, pkg.data);
                // 执行完任务释放data内存
                if (pkg.data != NULL) {
                    queFree(pkg.data);
                }
                // 任务执行后让出CPU，避免长时间占用
                taskYIELD();
                continue;
            }
        }
        
        // 没有任务或获取锁失败，延时等待
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    
    vTaskDelete(NULL);
}

int TaskQueStart(TaskQue* que, int cpu) {
    if (que == NULL) return -1;
    if (que->isRuning) return 0;
    
    que->isRuning = 1;
    
    BaseType_t ret;
    if (cpu < 0 || cpu >= portNUM_PROCESSORS) {
        // 不绑定特定CPU
        ret = xTaskCreate(TaskHandler, "TaskQue", 4096, que, 10, NULL);
    } else {
        // 绑定到指定CPU
        ret = xTaskCreatePinnedToCore(TaskHandler, "TaskQue", 4096, que, 10, NULL, cpu);
    }
    
    if (ret != pdPASS) {
        que->isRuning = 0;
        return -1;
    }
    
    return 0;
}

void TaskQueStop(TaskQue* que) {
    if (que == NULL) return;
    que->isRuning = 0;
    // 给一点时间让任务退出
    vTaskDelay(pdMS_TO_TICKS(100));
}

void DeleteTaskQue(TaskQue* que) {
    if (que == NULL) return;
    
    TaskQueStop(que);
    
    // 释放队列中剩余的任务数据（获取锁后操作）
    if (xSemaphoreTake(que->mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        while (que->head != que->tail) {
            TaskPackage* pkg = &que->ringBuf[que->head];
            if (pkg->data != NULL) {
                queFree(pkg->data);
            }
            que->head = (que->head + 1) % que->len;
        }
        xSemaphoreGive(que->mutex);
    }
    
    // 删除互斥锁
    if (que->mutex != NULL) {
        vSemaphoreDelete(que->mutex);
    }
    
    if (que->ringBuf != NULL) {
        queFree(que->ringBuf);
    }
    queFree(que);
}

int TaskQueAdd(TaskQue* que, TaskPackage pkg) {
    if (que == NULL || que->ringBuf == NULL) return 1;
    
    // 获取锁后操作队列
    if (xSemaphoreTake(que->mutex, pdMS_TO_TICKS(10)) != pdTRUE) {
        return 1;  // 获取锁失败，放弃添加
    }
    
    int nextTail = (que->tail + 1) % que->len;
    if (nextTail == que->head) {
        // 队列已满
        xSemaphoreGive(que->mutex);
        return 1;
    }
    
    que->ringBuf[que->tail] = pkg;
    que->tail = nextTail;
    xSemaphoreGive(que->mutex);
    return 0;
}
