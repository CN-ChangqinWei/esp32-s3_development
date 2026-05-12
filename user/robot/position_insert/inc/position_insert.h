#ifndef _POSITION_INSERT_H
#define _POSITION_INSERT_H

#include <stddef.h>

// ===== 释放函数类型定义 =====
typedef void (*PositionInsertFreeFunc)(void* p);

// ========== ① 接口表（虚函数表 V-Table）==========
typedef struct {
    // 插值：对输入数组进行插值，输出到 arryOut（空间由具体实现分配），outLen 返回输出长度
    void (*insert)(void* instance, void* arry, int len, void* arryOut, int* outLen);

    // 获取释放输出序列的函数指针
    PositionInsertFreeFunc (*getFreeArry)(void* instance);

    // 【必需】析构函数 - 销毁具体实例
    void (*deleteInstance)(void* p);
} PositionInsertInterface;

// ========== ② 抽象类结构体 ==========
typedef struct {
    void*                   instance;      // 指向具体实现的实例（多态核心）
    PositionInsertInterface interface;     // 接口函数表
} PositionInsert;

// ========== ③ 模块级内存管理接口 ==========
extern void* (*positionInsertMalloc)(size_t size);
extern void  (*positionInsertFree)(void* p);

// ========== ④ 抽象类构造函数 ==========
static inline PositionInsert* NewPositionInsert(void* instance, PositionInsertInterface interface) {
    if (NULL == instance) return NULL;
    PositionInsert* res = (PositionInsert*)positionInsertMalloc(sizeof(PositionInsert));
    if (NULL != res) {
        res->instance = instance;
        res->interface = interface;
    }
    return res;
}

// ========== ⑤ 抽象类析构函数 ==========
static inline void DeletePositionInsert(PositionInsert** insert) {
    if (NULL == insert || NULL == *insert) return;

    // ① 先调用具体实例的析构函数
    if (NULL != (*insert)->interface.deleteInstance) {
        (*insert)->interface.deleteInstance((*insert)->instance);
    }

    // ② 释放抽象类本身内存
    positionInsertFree(*insert);
    *insert = NULL;  // 防止悬空指针
}

#endif
