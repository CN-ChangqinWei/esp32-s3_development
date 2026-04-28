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
    if(repo == NULL||id<0) return 0;
    MotorRepo* r = (MotorRepo*)repo;
    if(r->len<=id) return 0;
    return 1;
}

int setPosition(void*repo,int id,int numAngel,int denAngel,int maxAngel){
    if(repo == NULL) return 0;
    MotorRepo* motorRepo = (MotorRepo*)repo;
    if(id < 0 || id >= motorRepo->len) return 0;
    return motorRepo->motors[id].interface.setPosition(motorRepo->motors[id].instance, numAngel, denAngel, maxAngel) == 0;
}

int setPositionByEncode(void* repo,int id,int encode){
    if(repo == NULL) return 0;
    MotorRepo* motorRepo = (MotorRepo*)repo;
    if(id < 0 || id >= motorRepo->len) return 0;
    return motorRepo->motors[id].interface.setPositionByEncode(motorRepo->motors[id].instance, encode) == 0;
}

int setPwm(void*repo,int id,int pwmNum,int pwmDen){
    if(repo == NULL) return 0;
    MotorRepo* motorRepo = (MotorRepo*)repo;
    if(id < 0 || id >= motorRepo->len) return 0;
    return motorRepo->motors[id].interface.setSpeedByPwm(motorRepo->motors[id].instance, pwmNum, pwmDen) == 0;
}

int setSpeedByAngel(void* repo,int id,int spNumAngel,int spDenAngel){
    if(repo == NULL) return 0;
    MotorRepo* motorRepo = (MotorRepo*)repo;
    if(id < 0 || id >= motorRepo->len) return 0;
    return motorRepo->motors[id].interface.setSpeedByAngel(motorRepo->motors[id].instance, spNumAngel, spDenAngel) == 0;
}

int powerOn(void* repo,int id){
    if(repo == NULL) return 0;
    MotorRepo* motorRepo = (MotorRepo*)repo;
    if(id < 0 || id >= motorRepo->len) return 0;

    return motorRepo->motors[id].interface.powerOn(motorRepo->motors[id].instance) == 0;
}

int shutOff(void* repo,int id){
    if(repo == NULL) return 0;
    MotorRepo* motorRepo = (MotorRepo*)repo;
    if(id < 0 || id >= motorRepo->len) return 0;
    return motorRepo->motors[id].interface.shutDown(motorRepo->motors[id].instance) == 0;
}
