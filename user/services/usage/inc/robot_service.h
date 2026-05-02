#ifndef _ROBOT_SERVICE_H
#define _ROBOT_SERVICE_H

#include <stdint.h>
#include "robot_domain.h"
#include "robot_position_resolve.h"
#include "freertos/FreeRTOS.h"
#include "three_axis_irb460.h"

// 电机位置参数（用于批量设置）
typedef struct {
    int id;
    int numAngel;
    int denAngel;
    int maxAngel;
} RobotMotorPositionParam;

// 电机仓储接口表（抽象）
typedef struct {
    int (*isMotorExists)(void* repo, int id);
    int (*setPosition)(void* repo, int id, int numAngel, int denAngel, int maxAngel);
    int (*powerOn)(void* repo, int id);
    int (*setBranchPositions)(void* repo, RobotMotorPositionParam* params, int num);
} RobotMotorRepoInterface;

typedef struct {
    void* repo;
    RobotMotorRepoInterface interface;
} RobotMotorRepoWrapper;

// Robot服务结构体
typedef struct {
    RobotPositionResolve* kinematics;  // 逆运动学解算器
    RobotMotorRepoWrapper motorRepo;   // 电机仓储包装器
    int motorNum;                      // 电机数量（通常为3）
    int numAngel;                      // 默认分子角度
    int denAngel;                      // 默认分母角度
    int maxAngel;                      // 默认最大角度
    AxisFloat* difs;
    AxisFloat* scales;
    int vectorLen;
} RobotService;

// 构造函数 + 业务入口
RobotService* NewRobotService(RobotPositionResolve* kinematics, void* motorRepo, 
                              RobotMotorRepoInterface motorInterface, int motorNum,
                              AxisFloat* difs,AxisFloat* scales,int vectorLen);
RobotResult RobotExec(void* service, void* arg);

#endif
