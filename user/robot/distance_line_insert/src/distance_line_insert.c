#include "distance_line_insert.h"
#include <math.h>

// ========== 具体类实例结构体 ==========
struct DistanceLineInsertInstance {
    AxisFloat maxDistance;
};

// ========== 静态辅助函数：计算两点间距离 ==========
static AxisFloat pointDistance(const DistanceLinePoint* a, const DistanceLinePoint* b) {
    AxisFloat dx = b->x - a->x;
    AxisFloat dy = b->y - a->y;
    AxisFloat dz = b->z - a->z;
    return sqrt(dx * dx + dy * dy + dz * dz);
}

// ========== 具体函数实现（静态化，不暴露）==========

// 析构函数
static void DistanceLineDelete(void* p) {
    if (NULL == p) return;
    positionInsertFree(p);
}

// 获取释放输出序列的函数
static PositionInsertFreeFunc DistanceLineGetFreeArry(void* instance) {
    (void)instance;
    return positionInsertFree;
}

// 插值实现
static void DistanceLineInsert(void* instance, void* arry, int len, void* arryOut, int* outLen) {
    if (NULL == instance || NULL == arry || NULL == arryOut || NULL == outLen) return;

    // 输入必须是 2 个点
    if (len < 2) {
        *outLen = 0;
        return;
    }

    DistanceLineInsertInstance* self = (DistanceLineInsertInstance*)instance;
    DistanceLinePoint* points = (DistanceLinePoint*)arry;

    DistanceLinePoint* a = &points[0];
    DistanceLinePoint* b = &points[1];

    // 计算总距离
    AxisFloat totalDist = pointDistance(a, b);
    if (totalDist <= 1e-12) {
        // 两点重合，直接输出一个点
        DistanceLinePoint* result = (DistanceLinePoint*)positionInsertMalloc(sizeof(DistanceLinePoint));
        if (NULL == result) return;
        result->x = a->x;
        result->y = a->y;
        result->z = a->z;
        *(DistanceLinePoint**)arryOut = result;
        *outLen = 1;
        return;
    }

    // 计算分段数：尽可能接近 maxDistance
    int numSegments;
    if (self->maxDistance <= 1e-12) {
        numSegments = 1;
    } else {
        numSegments = (int)ceil(totalDist / self->maxDistance);
        if (numSegments < 1) numSegments = 1;
    }

    // 输出点数为 段数 + 1
    int outCount = numSegments + 1;
    DistanceLinePoint* result = (DistanceLinePoint*)positionInsertMalloc(sizeof(DistanceLinePoint) * outCount);
    if (NULL == result) return;

    // 生成等间距点（沿直线）
    for (int i = 0; i <= numSegments; i++) {
        AxisFloat t = (AxisFloat)i / (AxisFloat)numSegments;  // 0.0 ~ 1.0
        result[i].x = a->x + (b->x - a->x) * t;
        result[i].y = a->y + (b->y - a->y) * t;
        result[i].z = a->z + (b->z - a->z) * t;
    }

    *(DistanceLinePoint**)arryOut = result;
    *outLen = outCount;
}

// ========== 接口表初始化 ==========
static const PositionInsertInterface distanceLineInsertInterface = {
    .insert         = DistanceLineInsert,
    .getFreeArry    = DistanceLineGetFreeArry,
    .deleteInstance = DistanceLineDelete
};

// ========== 创建函数 ==========
PositionInsert* NewDistanceLineInsert(AxisFloat maxDistance) {
    if (maxDistance <= 1e-12) return NULL;

    DistanceLineInsertInstance* instance = (DistanceLineInsertInstance*)positionInsertMalloc(sizeof(DistanceLineInsertInstance));
    if (NULL == instance) return NULL;

    instance->maxDistance = maxDistance;

    PositionInsert* insert = NewPositionInsert(instance, distanceLineInsertInterface);
    if (NULL == insert) {
        positionInsertFree(instance);
        return NULL;
    }

    return insert;
}
