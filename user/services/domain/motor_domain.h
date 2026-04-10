#ifndef _MOTOR_DOMAIN_H
#define _MOTOR_DOMAIN_H
#include <stdint.h>

typedef enum{
    PositionAngelMode=0,
    PositionEncodeMode,
    SpeedEncodeMode,
    SpeedAngelMode
}MotorMode;

typedef enum{
    Success=0,
    NoSuchDev,
    ArgErr,
    Fail
}MotorResult;

typedef struct{
    int protocol;
    int id;
    int powerOn;
    //位置角度量(一般舵机)
    uint32_t numAngel;
    uint32_t denAngel;
    uint32_t maxAngel;
    // 编码器量;
    int     encode;
    //编码器速度
    int     spEncode;
    //占空比
    int     pwmNum;
    int     pwmDen;
    
    //角速度
    int spNumAngel;
    int spDenAngel;
    MotorMode mode;
}MotorDomain;

typedef struct{
    int protocol;
    int res;
}MotorDomainReply;

#endif
