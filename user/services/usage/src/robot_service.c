#include "robot_service.h"
#include <math.h>
#include <string.h>
#include"service.h"
#include"esp_log.h"
// 默认电机角度参数
#define DEFAULT_NUM_ANGEL  180
#define DEFAULT_DEN_ANGEL  180
#define DEFAULT_MAX_ANGEL  180

RobotResult RobotExec(void* service, void* arg) {
    if (service == NULL || arg == NULL) return RobotArgErr;
    
    RobotService* svc = (RobotService*)service;
    RobotDomain* domain = (RobotDomain*)arg;
    
    // ① 参数校验
    if (svc->kinematics == NULL) {
        return RobotFail;
    }
    
    // ② 准备逆运动学输入
    AxisFloat input[THREE_AXIS_IRB460_INPUT_DIM] = {domain->x, domain->y, domain->z};
    AxisFloat output[THREE_AXIS_IRB460_OUTPUT_DIM] = {0};
    
    // ③ 调用逆运动学解算
    svc->kinematics->interface.inverse(svc->kinematics->instance, input, output);
    
    // ④ 检查解算结果是否有效（角度为0可能表示无解）
    for (int i = 0; i < svc->motorNum && i < THREE_AXIS_IRB460_OUTPUT_DIM; i++) {
        if (isnan(output[i]) || isinf(output[i])) {
            return RobotInvKinematicsFail;
        }
    }
    
    // ⑤ 通过repo设置电机角度
    if (svc->motorRepo.repo != NULL) {
        for (int i = 0; i < svc->motorNum && i < THREE_AXIS_IRB460_OUTPUT_DIM; i++) {
            // 检查电机是否存在
            if (!svc->motorRepo.interface.isMotorExists(svc->motorRepo.repo, i)) {
                continue;  // 跳过不存在的电机
            }
            
            // 将解算出的角度（弧度）转换为整数角度值
            // 假设 output 为弧度，转换为度数
            if(svc->scales)
            output[i]*=svc->scales[i];
            if(svc->difs)
            output[i]+=svc->difs[i];
            int angleDeg = (int)(output[i] * 180.0f / 3.14159265f);
            
            // 设置电机位置
            svc->motorRepo.interface.setPosition(svc->motorRepo.repo, i, 
                                                  angleDeg, svc->denAngel, svc->maxAngel);
            
            // 上电使能
            svc->motorRepo.interface.powerOn(svc->motorRepo.repo, i);
        }
    }
    
    return RobotSuccess;
}

RobotService* NewRobotService(RobotPositionResolve* kinematics, void* motorRepo,
                              RobotMotorRepoInterface motorInterface, int motorNum,AxisFloat* difs,AxisFloat* scales,int vectorLen) {
    if (kinematics == NULL || motorRepo == NULL) return NULL;
    
    RobotService* svc = (RobotService*)pvPortMalloc(sizeof(RobotService));
    memset(svc,0,sizeof(RobotService));
    if (svc == NULL) return NULL;
    
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
    }
    if(scales){
        svc->scales=serviceMalloc(sizeof(AxisFloat)*svc->vectorLen);
        memcpy(svc->scales,scales,sizeof(AxisFloat)*svc->vectorLen);
    }
    
    return svc;
}
