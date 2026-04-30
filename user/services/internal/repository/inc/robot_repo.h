#ifndef _ROBOT_REPO_H
#define _ROBOT_REPO_H

#include "robot_service.h"
#include "motor.h"

typedef struct {
    Motor* motors;           // 电机抽象类数组
    int len;                 // 电机数量
    RobotMotorRepoInterface interface;
} RobotRepo;

RobotRepo* NewRobotRepo(Motor* motors, int len);

// 静态接口函数（供接口表指向）
int RobotIsMotorExists(void* repo, int id);
int RobotSetPosition(void* repo, int id, int numAngel, int denAngel, int maxAngel);
int RobotPowerOn(void* repo, int id);

#endif
