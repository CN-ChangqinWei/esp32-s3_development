#include"motor_repo.h"


MotorRepo* NewMotorReop(Motor* motors,int len){
    MotorRepo* repo = (MotorRepo*)pvPortMalloc(sizeof(MotorRepo));
    if(repo == NULL) return NULL;
    repo->motors = motors;
    repo->len = len;
    // 初始化接口
    repo->interface.isMotorExsits = isMotorExsits;
    repo->interface.setPosition = setPosition;
    repo->interface.setPositionByEncode = setPositionByEncode;
    repo->interface.setPwm = setPwm;
    repo->interface.setSpeedByAngel = setSpeedByAngel;
    repo->interface.powerOn = powerOn;
    repo->interface.shutOff = shutOff;
    return repo;
}

int isMotorExsits(void*repo,int id){
    if(repo == NULL) return 0;
    MotorRepo* motorRepo = (MotorRepo*)repo;
    if(id < 0 || id >= motorRepo->len) return 0;
    Motor** motors = (Motor**)motorRepo->motors;
    return motors[id] != NULL && motors[id]->instance != NULL;
}

int setPosition(void*repo,int id,int numAngel,int denAngel,int maxAngel){
    if(repo == NULL) return 0;
    MotorRepo* motorRepo = (MotorRepo*)repo;
    if(id < 0 || id >= motorRepo->len) return 0;
    Motor** motors = (Motor**)motorRepo->motors;
    Motor* motor = motors[id];
    if(motor == NULL || motor->interface.setPosition == NULL) return 0;
    return motor->interface.setPosition(motor->instance, numAngel, denAngel, maxAngel) == 0;
}

int setPositionByEncode(void* repo,int id,int encode){
    if(repo == NULL) return 0;
    MotorRepo* motorRepo = (MotorRepo*)repo;
    if(id < 0 || id >= motorRepo->len) return 0;
    Motor** motors = (Motor**)motorRepo->motors;
    Motor* motor = motors[id];
    if(motor == NULL || motor->interface.setPositionByEncode == NULL) return 0;
    return motor->interface.setPositionByEncode(motor->instance, encode) == 0;
}

int setPwm(void*repo,int id,int pwmNum,int pwmDen){
    if(repo == NULL) return 0;
    MotorRepo* motorRepo = (MotorRepo*)repo;
    if(id < 0 || id >= motorRepo->len) return 0;
    Motor** motors = (Motor**)motorRepo->motors;
    Motor* motor = motors[id];
    if(motor == NULL || motor->interface.setSpeedByPwm == NULL) return 0;
    return motor->interface.setSpeedByPwm(motor->instance, pwmNum, pwmDen) == 0;
}

int setSpeedByAngel(void* repo,int id,int spNumAngel,int spDenAngel){
    if(repo == NULL) return 0;
    MotorRepo* motorRepo = (MotorRepo*)repo;
    if(id < 0 || id >= motorRepo->len) return 0;
    Motor** motors = (Motor**)motorRepo->motors;
    Motor* motor = motors[id];
    if(motor == NULL || motor->interface.setSpeedByAngel == NULL) return 0;
    return motor->interface.setSpeedByAngel(motor->instance, spNumAngel, spDenAngel) == 0;
}

int powerOn(void* repo,int id){
    if(repo == NULL) return 0;
    MotorRepo* motorRepo = (MotorRepo*)repo;
    if(id < 0 || id >= motorRepo->len) return 0;
    Motor** motors = (Motor**)motorRepo->motors;
    Motor* motor = motors[id];
    if(motor == NULL || motor->interface.powerOn == NULL) return 0;
    return motor->interface.powerOn(motor->instance) == 0;
}

int shutOff(void* repo,int id){
    if(repo == NULL) return 0;
    MotorRepo* motorRepo = (MotorRepo*)repo;
    if(id < 0 || id >= motorRepo->len) return 0;
    Motor** motors = (Motor**)motorRepo->motors;
    Motor* motor = motors[id];
    if(motor == NULL || motor->interface.shutDown == NULL) return 0;
    return motor->interface.shutDown(motor->instance) == 0;
}
