#include "robot_service.h"
#include <math.h>
#include <string.h>
#include "service.h"
#include "task_que.h"

static const char* TAG = "ROBOT_SVC";
// 默认电机角度参数
#define DEFAULT_NUM_ANGEL  180
#define DEFAULT_DEN_ANGEL  180
#define DEFAULT_MAX_ANGEL  180
#define DEFAULT_TASK_QUEUE_LEN  10

// ========== 局部数据结构：插值使用的三维点（与 DistanceLinePoint 内存布局一致）==========
typedef struct {
    AxisFloat x;
    AxisFloat y;
    AxisFloat z;
} _InterpPoint;

// ========== 执行单点逆运动学 + 电机控制 ==========
static uint16_t RobotExecSinglePoint(RobotService* svc, AxisFloat x, AxisFloat y, AxisFloat z) {
    _SERVICE_LOG(TAG, "RobotExecSinglePoint: x=%.2f, y=%.2f, z=%.2f", x, y, z);

    // ① 准备逆运动学输入
    AxisFloat input[THREE_AXIS_IRB460_INPUT_DIM] = {x, y, z};
    AxisFloat output[THREE_AXIS_IRB460_OUTPUT_DIM] = {0};

    // ② 调用逆运动学解算
    _SERVICE_LOG(TAG, "RobotExecSinglePoint: calling inverse kinematics");
    svc->kinematics->interface.inverse(svc->kinematics->instance, input, output);

    _SERVICE_LOG(TAG, "RobotExecSinglePoint: inverse result: alpha=%.2f, beta=%.2f, gamma=%.2f",
                 output[0], output[1], output[2]);

    // ③ 检查解算结果是否有效
    for (int i = 0; i < svc->motorNum && i < THREE_AXIS_IRB460_OUTPUT_DIM; i++) {
        if (isnan(output[i]) || isinf(output[i])) {
            _SERVICE_LOG(TAG, "RobotExecSinglePoint failed: inverse result invalid at index %d", i);
            return (uint16_t)RobotInvKinematicsFail;
        }
    }

    // ④ 通过repo批量设置电机角度
    if (svc->motorRepo.repo != NULL && svc->motorRepo.interface.setBranchPositions != NULL) {
        _SERVICE_LOG(TAG, "RobotExecSinglePoint: batch setting motor positions, motorNum=%d", svc->motorNum);

        RobotMotorPositionParam params[THREE_AXIS_IRB460_OUTPUT_DIM];
        int validCount = 0;

        for (int i = 0; i < svc->motorNum && i < THREE_AXIS_IRB460_OUTPUT_DIM; i++) {
            if (!svc->motorRepo.interface.isMotorExists(svc->motorRepo.repo, i)) {
                _SERVICE_LOG(TAG, "RobotExecSinglePoint: motor %d not exists, skip", i);
                continue;
            }

            AxisFloat angleRad = output[i];
            if (svc->scales) angleRad *= svc->scales[i];
            if (svc->difs)   angleRad += svc->difs[i];
            int angleDeg = (int)(angleRad * 180.0f / 3.14159265f);

            _SERVICE_LOG(TAG, "RobotExecSinglePoint: motor %d, angleDeg=%d", i, angleDeg);

            params[validCount].id = i;
            params[validCount].numAngel = angleDeg;
            params[validCount].denAngel = svc->denAngel;
            params[validCount].maxAngel = svc->maxAngel;
            validCount++;
        }

        if (validCount > 0) {
            int batchRes = svc->motorRepo.interface.setBranchPositions(svc->motorRepo.repo, params, validCount);
            _SERVICE_LOG(TAG, "RobotExecSinglePoint: batch setBranchPositions result=%d, count=%d", batchRes, validCount);
        }
    } else {
        _SERVICE_LOG(TAG, "RobotExecSinglePoint: motorRepo or setBranchPositions is NULL, skip motor control");
    }

    return (uint16_t)RobotSuccess;
}

// ========== 任务函数：插值 → 逆运动学 → 电机控制 ==========
static uint16_t RobotExecTaskFunc(void* instance, void* data) {
    if (instance == NULL || data == NULL) return (uint16_t)RobotArgErr;
    
    RobotService* svc = (RobotService*)instance;
    RobotDomain* domain = (RobotDomain*)data;
    
    _SERVICE_LOG(TAG, "RobotExecTask start: x=%.2f, y=%.2f, z=%.2f",
                 domain->x, domain->y, domain->z);
    
    if (svc->kinematics == NULL) {
        _SERVICE_LOG(TAG, "RobotExecTask failed: kinematics is NULL");
        return (uint16_t)RobotFail;
    }

    uint16_t result = (uint16_t)RobotSuccess;

    if (svc->positionInsert != NULL) {
        // ========== 使用插值器生成中间点序列 ==========
        _SERVICE_LOG(TAG, "RobotExecTask: positionInsert enabled, interpolating from (%.2f, %.2f, %.2f)",
                     svc->lastX, svc->lastY, svc->lastZ);

        _InterpPoint input[2] = {
            {svc->lastX, svc->lastY, svc->lastZ},
            {domain->x, domain->y, domain->z}
        };
        _InterpPoint* output = NULL;
        int outLen = 0;

        svc->positionInsert->interface.insert(svc->positionInsert->instance,
                                              input, 2, &output, &outLen);

        if (output != NULL && outLen > 0) {
            _SERVICE_LOG(TAG, "RobotExecTask: interpolated %d points", outLen);

            // 对每个插值点执行逆运动学 + 电机控制
            for (int i = 0; i < outLen; i++) {
                result = RobotExecSinglePoint(svc, output[i].x, output[i].y, output[i].z);
                if (result != (uint16_t)RobotSuccess) {
                    _SERVICE_LOG(TAG, "RobotExecTask: failed at interpolated point %d", i);
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(500));  // 点间间隔
            }

            // 释放插值结果
            PositionInsertFreeFunc freer = svc->positionInsert->interface.getFreeArry(svc->positionInsert->instance);
            if (freer != NULL) freer(output);
        } else {
            _SERVICE_LOG(TAG, "RobotExecTask: interpolation returned empty result, fallback to direct point");
            result = RobotExecSinglePoint(svc, domain->x, domain->y, domain->z);
        }

        // 更新上一个位置
        svc->lastX = domain->x;
        svc->lastY = domain->y;
        svc->lastZ = domain->z;
    } else {
        // ========== 无插值器，直接执行单点 ==========
        _SERVICE_LOG(TAG, "RobotExecTask: positionInsert is NULL, direct execution");
        result = RobotExecSinglePoint(svc, domain->x, domain->y, domain->z);
    }
    
    _SERVICE_LOG(TAG, "RobotExecTask %s", (result == (uint16_t)RobotSuccess) ? "success" : "failed");
    vTaskDelay(pdMS_TO_TICKS(2000));
    return result;
}

// ========== 对外接口：打包任务推入队列 ==========
RobotResult RobotExec(void* service, void* arg) {
    if (service == NULL || arg == NULL) {
        _SERVICE_LOG(TAG, "RobotExec failed: service or arg is NULL");
        return RobotArgErr;
    }
    
    RobotService* svc = (RobotService*)service;
    RobotDomain* domain = (RobotDomain*)arg;
    
    _SERVICE_LOG(TAG, "RobotExec queueing: x=%.2f, y=%.2f, z=%.2f",
                 domain->x, domain->y, domain->z);
    
    if (svc->kinematics == NULL) {
        _SERVICE_LOG(TAG, "RobotExec failed: kinematics is NULL");
        return RobotFail;
    }
    
    if (svc->taskQue == NULL) {
        _SERVICE_LOG(TAG, "RobotExec failed: taskQue is NULL");
        return RobotFail;
    }
    
    // 克隆 domain 数据（任务队列执行完后会自动释放）
    RobotDomain* clonedDomain = (RobotDomain*)serviceMalloc(sizeof(RobotDomain));
    if (clonedDomain == NULL) {
        _SERVICE_LOG(TAG, "RobotExec failed: malloc for cloned domain failed");
        return RobotFail;
    }
    memcpy(clonedDomain, domain, sizeof(RobotDomain));
    
    TaskPackage pkg = {
        .func = RobotExecTaskFunc,
        .instance = svc,
        .data = clonedDomain
    };
    
    int ret = TaskQueAdd(svc->taskQue, pkg);
    if (ret != 0) {
        _SERVICE_LOG(TAG, "RobotExec failed: task queue full");
        serviceFree(clonedDomain);
        return RobotFail;
    }
    
    _SERVICE_LOG(TAG, "RobotExec: task queued successfully");
    return RobotSuccess;
}

RobotService* NewRobotService(RobotPositionResolve* kinematics, PositionInsert* positionInsert,
                              void* motorRepo, RobotMotorRepoInterface motorInterface, int motorNum,
                              AxisFloat* difs, AxisFloat* scales, int vectorLen) {
    _SERVICE_LOG(TAG, "NewRobotService start: motorNum=%d, vectorLen=%d, positionInsert=%s",
                 motorNum, vectorLen, positionInsert ? "yes" : "no");
    
    if (kinematics == NULL || motorRepo == NULL) {
        _SERVICE_LOG(TAG, "NewRobotService failed: kinematics or motorRepo is NULL");
        return NULL;
    }
    
    RobotService* svc = (RobotService*)pvPortMalloc(sizeof(RobotService));
    if (svc == NULL) {
        _SERVICE_LOG(TAG, "NewRobotService failed: malloc failed");
        return NULL;
    }
    memset(svc, 0, sizeof(RobotService));
    
    svc->kinematics = kinematics;
    svc->positionInsert = positionInsert;
    svc->lastX = 0;
    svc->lastY = 50;
    svc->lastZ = 0;
    svc->motorRepo.repo = motorRepo;
    svc->motorRepo.interface = motorInterface;
    svc->motorNum = motorNum;
    svc->numAngel = DEFAULT_NUM_ANGEL;
    svc->denAngel = DEFAULT_DEN_ANGEL;
    svc->maxAngel = DEFAULT_MAX_ANGEL;
    svc->vectorLen = vectorLen;
    
    if (difs) {
        svc->difs = serviceMalloc(sizeof(AxisFloat) * svc->vectorLen);
        memcpy(svc->difs, difs, sizeof(AxisFloat) * svc->vectorLen);
        _SERVICE_LOG(TAG, "NewRobotService: difs copied");
    }
    if (scales) {
        svc->scales = serviceMalloc(sizeof(AxisFloat) * svc->vectorLen);
        memcpy(svc->scales, scales, sizeof(AxisFloat) * svc->vectorLen);
        _SERVICE_LOG(TAG, "NewRobotService: scales copied");
    }
    
    // 创建电机执行任务队列
    svc->taskQue = NewTaskQue(DEFAULT_TASK_QUEUE_LEN);
    if (svc->taskQue == NULL) {
        _SERVICE_LOG(TAG, "NewRobotService failed: create task queue failed");
        if (svc->difs) serviceFree(svc->difs);
        if (svc->scales) serviceFree(svc->scales);
        serviceFree(svc);
        return NULL;
    }
    
    // 启动任务队列（不绑定特定CPU，传入-1）
    if (TaskQueStart(svc->taskQue, -1) != 0) {
        _SERVICE_LOG(TAG, "NewRobotService failed: start task queue failed");
        DeleteTaskQue(svc->taskQue);
        if (svc->difs) serviceFree(svc->difs);
        if (svc->scales) serviceFree(svc->scales);
        serviceFree(svc);
        return NULL;
    }
    
    _SERVICE_LOG(TAG, "NewRobotService success");
    return svc;
}

// ========== 清理函数 ==========
void DeleteRobotService(RobotService* svc) {
    if (svc == NULL) return;
    
    _SERVICE_LOG(TAG, "DeleteRobotService");
    
    if (svc->taskQue != NULL) {
        DeleteTaskQue(svc->taskQue);
        svc->taskQue = NULL;
    }
    
    if (svc->difs) serviceFree(svc->difs);
    if (svc->scales) serviceFree(svc->scales);
    
    serviceFree(svc);
}
