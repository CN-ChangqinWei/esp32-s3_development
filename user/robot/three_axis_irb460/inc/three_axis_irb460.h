#ifndef _THREE_AXIS_IRB460_H
#define _THREE_AXIS_IRB460_H

#include "robot_position_resolve.h"
#include <math.h>

// 输入向量长度：3 (x, y, z)
#define THREE_AXIS_IRB460_INPUT_DIM  3
// 输出向量长度：3 (alpha, beta, gamma)
#define THREE_AXIS_IRB460_OUTPUT_DIM 3

// ========== 具体类实例结构体（对用户隐藏）==========
typedef struct {
    AxisFloat a;  // 连杆a长度
    AxisFloat b;  // 连杆b长度
    AxisFloat H;  // 底座高度
} ThreeAxisIrb460Instance;

// ========== 创建函数 ==========
// 参数：a, b, c 为三个连杆长度，H 为底座高度
RobotPositionResolve* NewThreeAxisIrb460(AxisFloat a, AxisFloat b, AxisFloat H);

#endif
