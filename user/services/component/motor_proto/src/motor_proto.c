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
    char* sendBufs[MAX_PROTO_GROUPS][8] = {{NULL}};  // 每个proto最多8个包
    int bufCounts[MAX_PROTO_GROUPS] = {0};
    int groupCount = 0;
    
    // ① 遍历所有电机，按 proto 分组
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
            groupCount++;
        }
        
        // 检查该组是否已满
        if (bufCounts[groupIdx] >= 8) continue;
        
        // 构造 MotorDomain 数据（格式：4字节长度 + 数据）
        int dataLen = sizeof(MotorDomain);
        int totalLen = sizeof(uint32_t) + dataLen;
        char* buf = (char*)motorMalloc(totalLen);
        if (buf == NULL) continue;
        
        // 写入长度（4字节）
        uint32_t len = (uint32_t)dataLen;
        memcpy(buf, &len, sizeof(uint32_t));
        
        // 从参数中获取 MotorDomain，如果存在则直接使用，否则构造新的
        if (motorDomains[i] != NULL) {
            // 直接复制传入的 MotorDomain
            memcpy(buf + sizeof(uint32_t), motorDomains[i], sizeof(MotorDomain));
        } else {
            // 构造默认的 MotorDomain
            MotorDomain* domain = (MotorDomain*)(buf + sizeof(uint32_t));
            domain->protocol = PROTO_MOTOR;
            domain->id = mp->id;
            domain->powerOn = 1;
            domain->mode = PositionAngelMode;
            domain->numAngel = 0;
            domain->denAngel = 180;
            domain->maxAngel = 180;
            domain->encode = 0;
            domain->pwmNum = 0;
            domain->pwmDen = 0;
            domain->spNumAngel = 0;
            domain->spDenAngel = 0;
            domain->spEncode = 0;
        }
        
        sendBufs[groupIdx][bufCounts[groupIdx]] = buf;
        bufCounts[groupIdx]++;
    }
    
    // ② 对每个 proto 组使用 sendBranchPackages 批量发送
    int totalSent = 0;
    for (int i = 0; i < groupCount; i++) {
        if (protoGroups[i] == NULL || bufCounts[i] == 0) continue;
        
        // 使用 proto 的 sendBranchPackages 发送
        ProtoInterface* interface = &protoGroups[i]->interfaces;
        if (interface->sendBranchPackages != NULL) {
            int sent = interface->sendBranchPackages(
                protoGroups[i]->instance, 
                sendBufs[i], 
                bufCounts[i]
            );
            if (sent > 0) {
                totalSent += sent;
            }
        }
        
        // 释放发送缓冲区
        for (int j = 0; j < bufCounts[i]; j++) {
            if (sendBufs[i][j] != NULL) {
                motorFree(sendBufs[i][j]);
            }
        }
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
