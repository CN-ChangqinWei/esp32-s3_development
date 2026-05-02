#include "robot_service.h"
#include <math.h>
#include <string.h>
#include "service.h"

static const char* TAG = "ROBOT_SVC";
// 默认电机角度参数
#define DEFAULT_NUM_ANGEL  180
#define DEFAULT_DEN_ANGEL  180
#define DEFAULT_MAX_ANGEL  180

RobotResult RobotExec(void* service, void* arg) {
    _SERVICE_LOG(TAG, "RobotExec start");
    
    if (service == NULL || arg == NULL) {
        _SERVICE_LOG(TAG, "RobotExec failed: service or arg is NULL");
        return RobotArgErr;
    }
    
    RobotService* svc = (RobotService*)service;
    RobotDomain* domain = (RobotDomain*)arg;
    
    _SERVICE_LOG(TAG, "RobotExec input: x=%.2f, y=%.2f, z=%.2f", domain->x, domain->y, domain->z);
    
    // ① 参数校验
    if (svc->kinematics == NULL) {
        _SERVICE_LOG(TAG, "RobotExec failed: kinematics is NULL");
        return RobotFail;
    }
    
    // ② 准备逆运动学输入
    AxisFloat input[THREE_AXIS_IRB460_INPUT_DIM] = {domain->x, domain->y, domain->z};
    AxisFloat output[THREE_AXIS_IRB460_OUTPUT_DIM] = {0};
    
    // ③ 调用逆运动学解算
    _SERVICE_LOG(TAG, "RobotExec: calling inverse kinematics");
    svc->kinematics->interface.inverse(svc->kinematics->instance, input, output);
    
    _SERVICE_LOG(TAG, "RobotExec: inverse result: alpha=%.2f, beta=%.2f, gamma=%.2f", 
                 output[0], output[1], output[2]);
    
    // ④ 检查解算结果是否有效（角度为0可能表示无解）
    for (int i = 0; i < svc->motorNum && i < THREE_AXIS_IRB460_OUTPUT_DIM; i++) {
        if (isnan(output[i]) || isinf(output[i])) {
            _SERVICE_LOG(TAG, "RobotExec failed: inverse kinematics result invalid at index %d", i);
            return RobotInvKinematicsFail;
        }
    }
    
    // ⑤ 通过repo批量设置电机角度
    if (svc->motorRepo.repo != NULL && svc->motorRepo.interface.setBranchPositions != NULL) {
        _SERVICE_LOG(TAG, "RobotExec: batch setting motor positions, motorNum=%d", svc->motorNum);
        
        // 准备批量设置参数
        RobotMotorPositionParam params[THREE_AXIS_IRB460_OUTPUT_DIM];
        int validCount = 0;
        
        for (int i = 0; i < svc->motorNum && i < THREE_AXIS_IRB460_OUTPUT_DIM; i++) {
            // 检查电机是否存在
            if (!svc->motorRepo.interface.isMotorExists(svc->motorRepo.repo, i)) {
                _SERVICE_LOG(TAG, "RobotExec: motor %d not exists, skip", i);
                continue;
            }
            
            // 将解算出的角度（弧度）转换为整数角度值
            AxisFloat angleRad = output[i];
            if(svc->scales) {
                angleRad *= svc->scales[i];
            }
            if(svc->difs) {
                angleRad += svc->difs[i];
            }
            int angleDeg = (int)(angleRad * 180.0f / 3.14159265f);
            
            _SERVICE_LOG(TAG, "RobotExec: motor %d, angleRad=%.2f, angleDeg=%d", i, angleRad, angleDeg);
            
            // 填充批量设置参数
            params[validCount].id = i;
            params[validCount].numAngel = angleDeg;
            params[validCount].denAngel = svc->denAngel;
            params[validCount].maxAngel = svc->maxAngel;
            validCount++;
        }
        
        // 批量设置电机位置
        if (validCount > 0) {
            int batchRes = svc->motorRepo.interface.setBranchPositions(svc->motorRepo.repo, params, validCount);
            _SERVICE_LOG(TAG, "RobotExec: batch setBranchPositions result=%d, count=%d", batchRes, validCount);
        }
    } else {
        _SERVICE_LOG(TAG, "RobotExec: motorRepo or setBranchPositions is NULL, skip motor control");
    }
    
    _SERVICE_LOG(TAG, "RobotExec success");
    return RobotSuccess;
}

RobotService* NewRobotService(RobotPositionResolve* kinematics, void* motorRepo,
                              RobotMotorRepoInterface motorInterface, int motorNum,AxisFloat* difs,AxisFloat* scales,int vectorLen) {
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
    memset(svc,0,sizeof(RobotService));
    
    svc->kinematics = kinematics;
    svc->motorRepo.repo = motorRepo;
    svc->motorRepo.interface = motorInterface;
    svc->motorNum = motorNum;
    svc->numAngel = DEFAULT_NUM_ANGEL;
    svc->denAngel = DEFAULT_DEN_ANGEL;
    svc->maxAngel = DEFAULT_MAX_ANGEL;
    svc->vectorLen=vectorLen;
    if(difs){
        svc->difs=serviceMalloc(sizeof(AxisFloat)*svc->vectorLen);
        memcpy(svc->difs,difs,sizeof(AxisFloat)*svc->vectorLen);
        _SERVICE_LOG(TAG, "NewRobotService: difs copied");
    }
    if(scales){
        svc->scales=serviceMalloc(sizeof(AxisFloat)*svc->vectorLen);
        memcpy(svc->scales,scales,sizeof(AxisFloat)*svc->vectorLen);
        _SERVICE_LOG(TAG, "NewRobotService: scales copied");
    }
    
    _SERVICE_LOG(TAG, "NewRobotService success");
    return svc;
}
