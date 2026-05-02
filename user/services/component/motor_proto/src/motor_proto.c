#include "motor_proto.h"
#include "freertos/FreeRTOS.h"
#include <string.h>
#include "multi_motor_domain.h"

// 静态函数声明
static int MotorProtoPowerOn(void* instance);
static int MotorProtoShutDown(void* instance);
static int MotorProtoSetPosition(void* instance, int numAngel, int denAngel, int maxAngel);
static int MotorProtoSetPositionByEncode(void* instance, int encode);
static int MotorProtoSetSpeedByPwm(void* instance, int pwmNum, int pwmDen);
static int MotorProtoSetSpeedByAngel(void* instance, int spNumAngel, int spDenAngel);
static int MotorProtoSetSpeedByEncode(void* instance, int encode);
static int MotorProtoSetBranchMotors(void* insArry,void** params,int num);

Motor* NewMotorProto(Proto* proto, int id) {
    if (proto == NULL||id<0) return NULL;
    
    MotorProto* motorProto = (MotorProto*)motorMalloc(sizeof(MotorProto));
    if (motorProto == NULL) return NULL;
    
    motorProto->proto = proto;
    motorProto->id = id;
    
    return NewMotor(motorProto, MotorProtoInterfaces());
}
void InitMotorProto(Motor* motor,Proto* proto,int id){
    if (motor == NULL || proto == NULL || id < 0) return;
    MotorProto*mp = (MotorProto*)motorMalloc(sizeof(MotorProto));
    memset(mp,0,sizeof(MotorProto));
    mp->id=id;
    mp->proto=proto;
    motor->instance=mp;
    motor->interface = MotorProtoInterfaces();

}
static int MotorProtoPowerOn(void* instance) {
    MotorProto* mp = (MotorProto*)instance;
    if (mp == NULL || mp->proto == NULL) return -1;
    return 0;
}

static int MotorProtoShutDown(void* instance) {
    MotorProto* mp = (MotorProto*)instance;
    if (mp == NULL || mp->proto == NULL) return -1;
    MotorDomain domain = {
        .protocol = PROTO_MOTOR,
        .id = mp->id,
        .powerOn=0
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
        .mode = PositionAngelMode,
        .powerOn =1
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
        .mode = PositionEncodeMode,
        .powerOn =1
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
        .mode = SpeedEncodeMode,
        .powerOn =1  // 使用编码器速度模式标识PWM模式
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
        .mode = SpeedAngelMode,
        .powerOn =1
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
        .mode = SpeedEncodeMode,
        .powerOn =1
    };
    
    ProtoSendPackage(mp->proto, (char*)&domain, sizeof(MotorDomain));
    return 0;
}

static int MotorProtoSetBranchMotors(void* insArry, void** params, int num) {
    if (insArry == NULL || params == NULL || num <= 0) return -1;
    
    // insArry 是 Motor* 结构体数组 (连续内存)
    Motor* motors = (Motor*)insArry;
    // params 是 MotorDomain* 数组
    MotorDomain** motorDomains = (MotorDomain**)params;
    
    // 最大支持的不同 proto 数量（可以根据实际需求调整）
    #define MAX_PROTO_GROUPS 4
    
    Proto* protoGroups[MAX_PROTO_GROUPS] = {NULL};
    char* sendBufs[MAX_PROTO_GROUPS] = {NULL};  // 每个proto一个发送缓冲区
    int motorCounts[MAX_PROTO_GROUPS] = {0};    // 每个proto的电机数量
    int groupCount = 0;
    
    // ① 遍历所有电机，按 proto 分组，统计每个proto的电机数量
    for (int i = 0; i < num; i++) {
        // motors 是结构体数组，使用 &motors[i] 获取每个 Motor 的地址
        if (motors[i].instance == NULL) continue;
        
        MotorProto* mp = (MotorProto*)motors[i].instance;
        Proto* currentProto = mp->proto;
        if (currentProto == NULL) continue;
        
        // 查找或创建 proto 组
        int groupIdx = -1;
        for (int j = 0; j < groupCount; j++) {
            if (protoGroups[j] == currentProto) {
                groupIdx = j;
                break;
            }
        }
        
        // 新 proto，创建新组
        if (groupIdx < 0) {
            if (groupCount >= MAX_PROTO_GROUPS) continue;  // 组满了，跳过
            groupIdx = groupCount;
            protoGroups[groupIdx] = currentProto;
            motorCounts[groupIdx] = 0;
            groupCount++;
        }
        
        motorCounts[groupIdx]++;
    }
    
    // ② 为每个 proto 组构造 MultiMotorDomain 数据包
    // 数据包格式：MultiMotorDomain + nums个MotorDomain
    for (int i = 0; i < groupCount; i++) {
        if (protoGroups[i] == NULL || motorCounts[i] == 0) continue;
        
        // 计算总数据长度
        int dataLen = sizeof(MultiMotorDomain) + motorCounts[i] * sizeof(MotorDomain);
        int totalLen = dataLen;  // 4字节长度 + 数据
        
        char* buf = (char*)motorMalloc(totalLen);
        if (buf == NULL) continue;
        
        // 写入长度（4字节）
        
        
        // 构造 MultiMotorDomain 头部
        MultiMotorDomain* multiDomain = (MultiMotorDomain*)(buf );
        multiDomain->protocol = PROTO_MULTI_MOTOR;  // 使用批量电机协议
        multiDomain->nums = motorCounts[i];
        
        // 填充 MotorDomain 数组
        MotorDomain* domains = (MotorDomain*)(buf  + sizeof(MultiMotorDomain));
        int domainIdx = 0;
        for (int j = 0; j < num && domainIdx < motorCounts[i]; j++) {
            if (motors[j].instance == NULL) continue;
            
            MotorProto* mp = (MotorProto*)motors[j].instance;
            if (mp->proto != protoGroups[i]) continue;
            
            // 从参数中获取 MotorDomain，如果存在则直接使用，否则构造新的
            if (motorDomains[j] != NULL) {
                memcpy(&domains[domainIdx], motorDomains[j], sizeof(MotorDomain));
            } else {
                domains[domainIdx].protocol = PROTO_MOTOR;
                domains[domainIdx].id = mp->id;
                domains[domainIdx].powerOn = 1;
                domains[domainIdx].mode = PositionAngelMode;
                domains[domainIdx].numAngel = 0;
                domains[domainIdx].denAngel = 180;
                domains[domainIdx].maxAngel = 180;
                domains[domainIdx].encode = 0;
                domains[domainIdx].pwmNum = 0;
                domains[domainIdx].pwmDen = 0;
                domains[domainIdx].spNumAngel = 0;
                domains[domainIdx].spDenAngel = 0;
                domains[domainIdx].spEncode = 0;
            }
            domainIdx++;
        }
        
        sendBufs[i] = buf;
    }
    
    // ③ 发送每个 proto 组的数据包（单包发送）
    int totalSent = 0;
    for (int i = 0; i < groupCount; i++) {
        if (protoGroups[i] == NULL || sendBufs[i] == NULL) continue;
        
        // 计算数据长度（不包含4字节长度字段本身）
        int dataLen = sizeof(MultiMotorDomain) + motorCounts[i] * sizeof(MotorDomain);
        int totalLen = sizeof(uint32_t) + dataLen;
        
        // 使用 ProtoSendPackage 发送
        ProtoSendPackage(protoGroups[i], sendBufs[i], totalLen);
        totalSent += totalLen;
        
        // 释放发送缓冲区
        motorFree(sendBufs[i]);
    }
    
    return totalSent > 0 ? 0 : -1;
}

MotorInterface MotorProtoInterfaces() {
    MotorInterface interface = {
        .powerOn = MotorProtoPowerOn,
        .shutDown = MotorProtoShutDown,
        .setPosition = MotorProtoSetPosition,
        .setPositionByEncode = MotorProtoSetPositionByEncode,
        .setSpeedByPwm = MotorProtoSetSpeedByPwm,
        .setSpeedByAngel = MotorProtoSetSpeedByAngel,
        .setSpeedByEncode = MotorProtoSetSpeedByEncode,
        .setBranchMotors = MotorProtoSetBranchMotors
    };
    return interface;
}
