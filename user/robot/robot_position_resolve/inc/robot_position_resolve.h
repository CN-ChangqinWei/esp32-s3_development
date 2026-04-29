#ifndef _ROBOT_POSITION_RESOLVE_H
#define _ROBOT_POSITION_RESOLVE_H

#include "platform.h"

// ========== ① 接口表（虚函数表 V-Table）==========
typedef struct {
    // 虚函数：第一个参数统一为 void* instance（this指针）
    void (*inverse)(void* instance, AxisFloat* in, AxisFloat* out);
    
    // 【必需】析构函数 - 销毁具体实例
    void (*deleteInstance)(void* p);
} RobotPositionInterface;

// ========== ② 抽象类结构体 ==========
typedef struct {
    void* instance;                    // 指向具体实现的实例（多态核心）
    RobotPositionInterface interface;  // 接口函数表
} RobotPositionResolve;

// ========== ③ 抽象类构造函数 ==========
static inline RobotPositionResolve* NewRobotPositionResolve(void* instance, RobotPositionInterface interface) {
    if (NULL == instance) return NULL;
    RobotPositionResolve* res = (RobotPositionResolve*)robotMalloc(sizeof(RobotPositionResolve));
    if (NULL != res) {
        res->instance = instance;
        res->interface = interface;
    }
    return res;
}

// ========== ④ 抽象类析构函数 ==========
static inline void DeleteRobotPositionResolve(RobotPositionResolve** resolve) {
    if (NULL == resolve || NULL == *resolve) return;
    
    // ① 先调用具体实例的析构函数
    if (NULL != (*resolve)->interface.deleteInstance) {
        (*resolve)->interface.deleteInstance((*resolve)->instance);
    }
    
    // ② 释放抽象类本身内存
    robotFree(*resolve);
    *resolve = NULL;  // 防止悬空指针
}

#endif