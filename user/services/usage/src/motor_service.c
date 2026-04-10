#include"motor_service.h"

#include<stdio.h>

int MotorExec(void* service,void* arg){//根据mode实现各种电机操作
    if(service == NULL || arg == NULL) return Fail;
    
    MotorService* motorService = (MotorService*)service;
    MotorDomain* domain = (MotorDomain*)arg;
    static MotorDomainReply reply;
    
    
    
    void* repo = motorService->repo;
    int id = domain->id;
    
    // 检查电机是否存在
    if(!motorService->interface.isMotorExsits(repo, id)){
        
        return NoSuchDev;
    }
    
    // 根据电源状态操作
    if(domain->powerOn){
        if(!motorService->interface.powerOn(repo, id)){
            return Fail;
        }
    } else {
        if(!motorService->interface.shutOff(repo, id)){
            return Fail;
        }
        
        return Success;
    }
    
    // 根据模式执行相应操作
    char result = 0;
    switch(domain->mode){
        case PositionAngelMode:
            result = motorService->interface.setPosition(repo, id, 
                domain->numAngel, domain->denAngel, domain->maxAngel);
            break;
        case PositionEncodeMode:
            result = motorService->interface.setPositionByEncode(repo, id, domain->encode);
            break;
        case SpeedEncodeMode:
            result = motorService->interface.setPwm(repo, id, domain->pwmNum, domain->pwmDen);
            break;
        case SpeedAngelMode:
            result = motorService->interface.setSpeedByAngel(repo, id, domain->spNumAngel, domain->spDenAngel);
            break;
        default:
            return ArgErr;
    }
    
    return Success;
}

MotorService* NewMotorService(void* repo,MotorRepoInterface interface){
    if(repo == NULL) return NULL;
    MotorService* service = (MotorService*)pvPortMalloc(sizeof(MotorService));
    if(service == NULL) return NULL;
    service->repo = repo;
    service->interface = interface;
    return service;
}