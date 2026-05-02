#include "robot_repo.h"
#include "freertos/FreeRTOS.h"
#include "motor_domain.h"
#include "protocol.h"
RobotRepo* NewRobotRepo(Motor* motors, int len) {
    if (motors == NULL || len <= 0) return NULL;
    
    RobotRepo* repo = (RobotRepo*)pvPortMalloc(sizeof(RobotRepo));
    if (repo == NULL) return NULL;
    
    repo->motors = motors;
    repo->len = len;
    
    // 初始化接口表
    repo->interface.isMotorExists = RobotIsMotorExists;
    repo->interface.setPosition = RobotSetPosition;
    repo->interface.powerOn = RobotPowerOn;
    repo->interface.setBranchPositions = RobotSetBranchPositions;
    
    return repo;
}

int RobotIsMotorExists(void* repo, int id) {
    if (repo == NULL || id < 0) return 0;
    RobotRepo* r = (RobotRepo*)repo;
    return id < r->len;
}

int RobotSetPosition(void* repo, int id, int numAngel, int denAngel, int maxAngel) {
    if (repo == NULL) return 0;
    RobotRepo* r = (RobotRepo*)repo;
    if (id < 0 || id >= r->len) return 0;
    
    // 调用抽象类接口（多态）
    return r->motors[id].interface.setPosition(
        r->motors[id].instance, numAngel, denAngel, maxAngel) == 0;
}

int RobotPowerOn(void* repo, int id) {
    if (repo == NULL) return 0;
    RobotRepo* r = (RobotRepo*)repo;
    if (id < 0 || id >= r->len) return 0;
    
    // 调用抽象类接口（多态）
    return r->motors[id].interface.powerOn(r->motors[id].instance) == 0;
}

int RobotSetBranchPositions(void* repo, RobotMotorPositionParam* params, int num) {
    if (repo == NULL || params == NULL || num <= 0) return 0;
    RobotRepo* r = (RobotRepo*)repo;
    
    // 构造 MotorDomain 数组作为参数
    MotorDomain* domains[8];  // 最多支持8个电机
    int validNum = num < 8 ? num : 8;
    
    for (int i = 0; i < validNum; i++) {
        domains[i] = (MotorDomain*)pvPortMalloc(sizeof(MotorDomain));
        if (domains[i] == NULL) continue;
        
        domains[i]->protocol = PROTO_MOTOR;
        domains[i]->id = params[i].id;
        domains[i]->powerOn = 1;
        domains[i]->mode = PositionAngelMode;
        domains[i]->numAngel = params[i].numAngel;
        domains[i]->denAngel = params[i].denAngel;
        domains[i]->maxAngel = params[i].maxAngel;
        domains[i]->encode = 0;
        domains[i]->pwmNum = 0;
        domains[i]->pwmDen = 0;
        domains[i]->spNumAngel = 0;
        domains[i]->spDenAngel = 0;
        domains[i]->spEncode = 0;
    }
    
    int result = 0;
    
    // 检查第一个电机是否支持批量设置
    if (r->len > 0 && r->motors[0].interface.setBranchMotors != NULL) {
        // 使用批量设置方式
        result = r->motors[0].interface.setBranchMotors(r->motors, (void**)domains, validNum) == 0;
    } else {
        // 不支持批量设置，逐个设置
        int success = 0;
        for (int i = 0; i < validNum; i++) {
            if (domains[i] == NULL) continue;
            if (domains[i]->id < 0 || domains[i]->id >= r->len) continue;
            
            int res = r->motors[domains[i]->id].interface.setPosition(
                r->motors[domains[i]->id].instance,
                domains[i]->numAngel,
                domains[i]->denAngel,
                domains[i]->maxAngel
            );
            if (res == 0) success++;
            
            // 上电使能
            r->motors[domains[i]->id].interface.powerOn(r->motors[domains[i]->id].instance);
        }
        result = success > 0;
    }
    
    // 释放分配的内存
    for (int i = 0; i < validNum; i++) {
        if (domains[i] != NULL) {
            vPortFree(domains[i]);
        }
    }
    
    return result;
}
