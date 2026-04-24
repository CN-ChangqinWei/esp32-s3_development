#include "motor_proto.h"
#include "freertos/FreeRTOS.h"
#include <string.h>

// 静态函数声明
static int MotorProtoPowerOn(void* instance);
static int MotorProtoShutDown(void* instance);
static int MotorProtoSetPosition(void* instance, int numAngel, int denAngel, int maxAngel);
static int MotorProtoSetPositionByEncode(void* instance, int encode);
static int MotorProtoSetSpeedByPwm(void* instance, int pwmNum, int pwmDen);
static int MotorProtoSetSpeedByAngel(void* instance, int spNumAngel, int spDenAngel);
static int MotorProtoSetSpeedByEncode(void* instance, int encode);

Motor* NewMotorProto(Proto* proto, int id) {
    if (proto == NULL||id<0) return NULL;
    
    MotorProto* motorProto = (MotorProto*)motorMalloc(sizeof(MotorProto));
    if (motorProto == NULL) return NULL;
    
    motorProto->proto = proto;
    motorProto->id = id;
    
    return NewMotor(motorProto, MotorProtoInterfaces());
}

static int MotorProtoPowerOn(void* instance) {
    MotorProto* mp = (MotorProto*)instance;
    if (mp == NULL || mp->proto == NULL) return -1;
    
    MotorDomain domain = {
        .protocol = PROTO_MOTOR,  // 假设 PROTO_MOTOR 已定义
        .id = mp->id,
        .powerOn = 1
    };
    
    ProtoSendPackage(mp->proto, (char*)&domain, sizeof(MotorDomain));
    return 0;
}

static int MotorProtoShutDown(void* instance) {
    MotorProto* mp = (MotorProto*)instance;
    if (mp == NULL || mp->proto == NULL) return -1;
    
    MotorDomain domain = {
        .protocol = PROTO_MOTOR,
        .id = mp->id,
        .powerOn = 0
    };
    
    ProtoSendPackage(mp->proto, (char*)&domain, sizeof(MotorDomain));
    return 0;
}

static int MotorProtoSetPosition(void* instance, int numAngel, int denAngel, int maxAngel) {
    MotorProto* mp = (MotorProto*)instance;
    if (mp == NULL || mp->proto == NULL) return -1;
    
    MotorDomain domain = {
        .protocol = PROTO_MOTOR,
        .id = mp->id,
        .numAngel = numAngel,
        .denAngel = denAngel,
        .maxAngel = maxAngel,
        .mode = PositionAngelMode
    };
    
    ProtoSendPackage(mp->proto, (char*)&domain, sizeof(MotorDomain));
    return 0;
}

static int MotorProtoSetPositionByEncode(void* instance, int encode) {
    MotorProto* mp = (MotorProto*)instance;
    if (mp == NULL || mp->proto == NULL) return -1;
    
    MotorDomain domain = {
        .protocol = PROTO_MOTOR,
        .id = mp->id,
        .encode = encode,
        .mode = PositionEncodeMode
    };
    
    ProtoSendPackage(mp->proto, (char*)&domain, sizeof(MotorDomain));
    return 0;
}

static int MotorProtoSetSpeedByPwm(void* instance, int pwmNum, int pwmDen) {
    MotorProto* mp = (MotorProto*)instance;
    if (mp == NULL || mp->proto == NULL) return -1;
    
    MotorDomain domain = {
        .protocol = PROTO_MOTOR,
        .id = mp->id,
        .pwmNum = pwmNum,
        .pwmDen = pwmDen,
        .mode = SpeedEncodeMode  // 使用编码器速度模式标识PWM模式
    };
    
    ProtoSendPackage(mp->proto, (char*)&domain, sizeof(MotorDomain));
    return 0;
}

static int MotorProtoSetSpeedByAngel(void* instance, int spNumAngel, int spDenAngel) {
    MotorProto* mp = (MotorProto*)instance;
    if (mp == NULL || mp->proto == NULL) return -1;
    
    MotorDomain domain = {
        .protocol = PROTO_MOTOR,
        .id = mp->id,
        .spNumAngel = spNumAngel,
        .spDenAngel = spDenAngel,
        .mode = SpeedAngelMode
    };
    
    ProtoSendPackage(mp->proto, (char*)&domain, sizeof(MotorDomain));
    return 0;
}

static int MotorProtoSetSpeedByEncode(void* instance, int encode) {
    MotorProto* mp = (MotorProto*)instance;
    if (mp == NULL || mp->proto == NULL) return -1;
    
    MotorDomain domain = {
        .protocol = PROTO_MOTOR,
        .id = mp->id,
        .spEncode = encode,
        .mode = SpeedEncodeMode
    };
    
    ProtoSendPackage(mp->proto, (char*)&domain, sizeof(MotorDomain));
    return 0;
}

MotorInterface MotorProtoInterfaces() {
    MotorInterface interface = {
        .powerOn = MotorProtoPowerOn,
        .shutDown = MotorProtoShutDown,
        .setPosition = MotorProtoSetPosition,
        .setPositionByEncode = MotorProtoSetPositionByEncode,
        .setSpeedByPwm = MotorProtoSetSpeedByPwm,
        .setSpeedByAngel = MotorProtoSetSpeedByAngel,
        .setSpeedByEncode = MotorProtoSetSpeedByEncode
    };
    return interface;
}
