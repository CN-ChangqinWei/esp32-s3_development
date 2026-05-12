#ifndef _DISTANCE_LINE_INSERT_H
#define _DISTANCE_LINE_INSERT_H

#include "position_insert.h"
#include "platform.h"

// ========== 坐标点结构体 ==========
typedef struct {
    AxisFloat x;
    AxisFloat y;
    AxisFloat z;
} DistanceLinePoint;

// ========== 具体类实例结构体（对用户隐藏，仅类型前置声明）==========
typedef struct DistanceLineInsertInstance DistanceLineInsertInstance;

// ========== 创建函数 ==========
// maxDistance：两点之间的最大允许距离，插值后相邻点间距尽量接近此值
PositionInsert* NewDistanceLineInsert(AxisFloat maxDistance);

#endif
