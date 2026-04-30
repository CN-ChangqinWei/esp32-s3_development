#ifndef _ROBOT_DOMAIN_H
#define _ROBOT_DOMAIN_H

#include <stdint.h>
#include "platform.h"

// ① 状态/结果枚举
typedef enum {
    RobotSuccess = 0,
    RobotArgErr,
    RobotFail,
    RobotInvKinematicsFail
} RobotResult;

// ② 领域实体（纯数据结构）- 三维坐标
typedef struct {
    int protocol;
    AxisFloat x;  // X坐标
    AxisFloat y;  // Y坐标
    AxisFloat z;  // Z坐标
} RobotDomain;

// ③ 响应结构
typedef struct {
    int protocol;
    int res;
} RobotDomainReply;

#endif
