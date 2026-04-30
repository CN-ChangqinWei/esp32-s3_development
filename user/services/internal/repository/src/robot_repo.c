#include "robot_repo.h"
#include "freertos/FreeRTOS.h"

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
