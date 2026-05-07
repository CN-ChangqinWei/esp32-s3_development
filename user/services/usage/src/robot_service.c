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

// ========== 任务函数：实际执行逆运动学 + 电机控制 ==========
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
    
    // ① 准备逆运动学输入
    AxisFloat input[THREE_AXIS_IRB460_INPUT_DIM] = {domain->x, domain->y, domain->z};
    AxisFloat output[THREE_AXIS_IRB460_OUTPUT_DIM] = {0};
    
    // ② 调用逆运动学解算
    _SERVICE_LOG(TAG, "RobotExecTask: calling inverse kinematics");
    svc->kinematics->interface.inverse(svc->kinematics->instance, input, output);
    
    _SERVICE_LOG(TAG, "RobotExecTask: inverse result: alpha=%.2f, beta=%.2f, gamma=%.2f",
                 output[0], output[1], output[2]);
    
    // ③ 检查解算结果是否有效
    for (int i = 0; i < svc->motorNum && i < THREE_AXIS_IRB460_OUTPUT_DIM; i++) {
        if (isnan(output[i]) || isinf(output[i])) {
            _SERVICE_LOG(TAG, "RobotExecTask failed: inverse result invalid at index %d", i);
            return (uint16_t)RobotInvKinematicsFail;
        }
    }
    
    // ④ 通过repo批量设置电机角度
    if (svc->motorRepo.repo != NULL && svc->motorRepo.interface.setBranchPositions != NULL) {
        _SERVICE_LOG(TAG, "RobotExecTask: batch setting motor positions, motorNum=%d", svc->motorNum);
        
        RobotMotorPositionParam params[THREE_AXIS_IRB460_OUTPUT_DIM];
        int validCount = 0;
        
        for (int i = 0; i < svc->motorNum && i < THREE_AXIS_IRB460_OUTPUT_DIM; i++) {
            if (!svc->motorRepo.interface.isMotorExists(svc->motorRepo.repo, i)) {
                _SERVICE_LOG(TAG, "RobotExecTask: motor %d not exists, skip", i);
                continue;
            }
            
            AxisFloat angleRad = output[i];
            if (svc->scales) angleRad *= svc->scales[i];
            if (svc->difs)   angleRad += svc->difs[i];
            int angleDeg = (int)(angleRad * 180.0f / 3.14159265f);
            
            _SERVICE_LOG(TAG, "RobotExecTask: motor %d, angleDeg=%d", i, angleDeg);
            
            params[validCount].id = i;
            params[validCount].numAngel = angleDeg;
            params[validCount].denAngel = svc->denAngel;
            params[validCount].maxAngel = svc->maxAngel;
            validCount++;
        }
        
        if (validCount > 0) {
            int batchRes = svc->motorRepo.interface.setBranchPositions(svc->motorRepo.repo, params, validCount);
            _SERVICE_LOG(TAG, "RobotExecTask: batch setBranchPositions result=%d, count=%d", batchRes, validCount);
        }
    } else {
        _SERVICE_LOG(TAG, "RobotExecTask: motorRepo or setBranchPositions is NULL, skip motor control");
    }
    
    _SERVICE_LOG(TAG, "RobotExecTask success");
    vTaskDelay(pdMS_TO_TICKS(2000));
    return (uint16_t)RobotSuccess;
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

RobotService* NewRobotService(RobotPositionResolve* kinematics, void* motorRepo,
                              RobotMotorRepoInterface motorInterface, int motorNum,
                              AxisFloat* difs, AxisFloat* scales, int vectorLen) {
    _SERVICE_LOG(TAG, "NewRobotService start: motorNum=%d, vectorLen=%d", motorNum, vectorLen);
    
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
