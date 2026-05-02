#include "json_proto.h"
#include <string.h>
#include "cJSON.h"


static void* (*jsonProtoMalloc)(size_t size) = pvPortMalloc;
static void (*jsonProtoFree)(void* p) = vPortFree;

// 创建 JsonProto 实例
Proto* NewJsonProto(Communication* comm, SerializeInterface* interfacesArray, int len) {
    if (comm == NULL || interfacesArray == NULL || len <= 0) return NULL;
    
    JsonProto* jsonProto = (JsonProto*)jsonProtoMalloc(sizeof(JsonProto));
    if (jsonProto == NULL) return NULL;
    
    jsonProto->comm = comm;
    jsonProto->interfacesArray = interfacesArray;
    jsonProto->interfacesLen = len;
    
    // 接收缓冲区初始化
    jsonProto->recvBufSize = 1024;
    jsonProto->recvBuf = (char*)jsonProtoMalloc(jsonProto->recvBufSize);
    if (jsonProto->recvBuf == NULL) {
        jsonProtoFree(jsonProto);
        return NULL;
    }
    jsonProto->recvLen = 0;
    jsonProto->braceCount = 0;
    jsonProto->inJson = 0;
    
    ProtoInterface interface = JsonProtoInterface();
    Proto* proto = NewProto(jsonProto, interface);
    if (proto == NULL) {
        jsonProtoFree(jsonProto->recvBuf);
        jsonProtoFree(jsonProto);
        return NULL;
    }
    
    return proto;
}

// 删除 JsonProto 实例
void DeleteJsonProto(void* p) {
    if (p == NULL) return;
    JsonProto* jsonProto = (JsonProto*)p;
    if (jsonProto->recvBuf != NULL) {
        jsonProtoFree(jsonProto->recvBuf);
    }
    jsonProtoFree(jsonProto);
}

// 扩展接收缓冲区
static int ExpandRecvBuf(JsonProto* jsonProto) {
    int newSize = jsonProto->recvBufSize * 2;
    char* newBuf = (char*)jsonProtoMalloc(newSize);
    if (newBuf == NULL) return 1;
    
    memcpy(newBuf, jsonProto->recvBuf, jsonProto->recvLen);
    jsonProtoFree(jsonProto->recvBuf);
    jsonProto->recvBuf = newBuf;
    jsonProto->recvBufSize = newSize;
    return 0;
}

// 提取 protocol 字段值
static int ExtractProtocol(char* jsonStr) {
    cJSON* root = cJSON_Parse(jsonStr);
    if (root == NULL) return -1;
    
    cJSON* item = cJSON_GetObjectItem(root, "protocol");
    int protocol = -1;
    if (cJSON_IsNumber(item)) {
        protocol = (int)cJSON_GetNumberValue(item);
    }
    cJSON_Delete(root);
    return protocol;
}

// 接收 JSON 包
void* JsonProtoProtoRecvPackage(void* p, int* len) {
    if (p == NULL) return NULL;
    if (len != NULL) *len = 0;
    
    JsonProto* jsonProto = (JsonProto*)p;
    if (jsonProto->comm == NULL) return NULL;
    
    // 临时缓冲区接收数据
    char tempBuf[256];
    uint32_t recvBytes = CommRecv(jsonProto->comm, tempBuf, sizeof(tempBuf));
    if (recvBytes == 0) return NULL;
    
    // 检查是否需要扩展缓冲区
    while (jsonProto->recvLen + recvBytes > jsonProto->recvBufSize) {
        if (ExpandRecvBuf(jsonProto) != 0) return NULL;
    }
    
    // 追加到接收缓冲区
    memcpy(jsonProto->recvBuf + jsonProto->recvLen, tempBuf, recvBytes);
    jsonProto->recvLen += recvBytes;
    jsonProto->recvBuf[jsonProto->recvLen] = '\0';
    
    // 解析 JSON 大括号匹配
    for (int i = jsonProto->recvLen - recvBytes; i < jsonProto->recvLen; i++) {
        char c = jsonProto->recvBuf[i];
        if (c == '{') {
            if (jsonProto->braceCount == 0) {
                jsonProto->inJson = 1;
                jsonProto->jsonStart = i;
            }
            jsonProto->braceCount++;
        } else if (c == '}' && jsonProto->inJson) {
            jsonProto->braceCount--;
            if (jsonProto->braceCount == 0) {
                // 找到完整 JSON
                int jsonLen = i - jsonProto->jsonStart + 1;
                char* jsonStr = (char*)jsonProtoMalloc(jsonLen + 1);
                if (jsonStr == NULL) return NULL;
                
                memcpy(jsonStr, jsonProto->recvBuf + jsonProto->jsonStart, jsonLen);
                jsonStr[jsonLen] = '\0';
                
                // 提取 protocol 值
                int protocol = ExtractProtocol(jsonStr);
                
                // 检查 protocol 合法性
                if (protocol < 0 || protocol >= jsonProto->interfacesLen) {
                    jsonProtoFree(jsonStr);
                    // 移动剩余数据
                    int remaining = jsonProto->recvLen - (i + 1);
                    if (remaining > 0) {
                        memmove(jsonProto->recvBuf, jsonProto->recvBuf + i + 1, remaining);
                    }
                    jsonProto->recvLen = remaining;
                    jsonProto->inJson = 0;
                    return NULL;
                }
                
                // 调用对应的反序列化函数
                SerializeInterface* si = &jsonProto->interfacesArray[protocol];
                void* package = NULL;
                if (si->reserialize != NULL) {
                    package = si->reserialize(jsonStr);
                }
                
                jsonProtoFree(jsonStr);
                
                // 移动剩余数据
                int remaining = jsonProto->recvLen - (i + 1);
                if (remaining > 0) {
                    memmove(jsonProto->recvBuf, jsonProto->recvBuf + i + 1, remaining);
                }
                jsonProto->recvLen = remaining;
                jsonProto->inJson = 0;
                
                if (len != NULL) {
                    *len = sizeof(void*);  // 返回指针大小
                }
                return package;
            }
        }
    }
    
    return NULL;
}

// 发送 JSON 包
int JsonProtoSendPackage(void* p, char* data, int len) {
    if (p == NULL || data == NULL || len < (int)sizeof(int)) return 0;
    
    JsonProto* jsonProto = (JsonProto*)p;
    if (jsonProto->comm == NULL) return 0;
    
    // 从前4字节获取协议值
    int protocol = *(int*)data;
    
    // 检查协议合法性
    if (protocol < 0 || protocol >= jsonProto->interfacesLen) return 0;
    
    SerializeInterface* si = &jsonProto->interfacesArray[protocol];
    if (si->serialize == NULL) return 0;
    
    // 调用对应的序列化函数
    char* jsonStr = si->serialize(data);
    if (jsonStr == NULL) return 0;
    
    // 发送 JSON 字符串
    int jsonLen = strlen(jsonStr);
    CommSend(jsonProto->comm, jsonStr, jsonLen);
    
    jsonProtoFree(jsonStr);
    return len;
}

// 批量发送多个 JSON 数据包
int JsonProtoSendBranchPackages(void* p, char** bufs, int num) {
    if (p == NULL || bufs == NULL || num <= 0) return 0;
    
    JsonProto* jsonProto = (JsonProto*)p;
    if (jsonProto->comm == NULL) return 0;
    
    int totalSent = 0;
    for (int i = 0; i < num; i++) {
        if (bufs[i] == NULL) continue;
        
        // 从前4字节获取协议值
        int protocol = *(int*)bufs[i];
        
        // 检查协议合法性
        if (protocol < 0 || protocol >= jsonProto->interfacesLen) continue;
        
        SerializeInterface* si = &jsonProto->interfacesArray[protocol];
        if (si->serialize == NULL) continue;
        
        // 调用对应的序列化函数
        char* jsonStr = si->serialize(bufs[i]);
        if (jsonStr == NULL) continue;
        
        // 发送 JSON 字符串
        int jsonLen = strlen(jsonStr);
        CommSend(jsonProto->comm, jsonStr, jsonLen);
        totalSent += jsonLen;
        
        jsonProtoFree(jsonStr);
    }
    
    return totalSent;
}

// 获取 ProtoInterface
ProtoInterface JsonProtoInterface(void) {
    ProtoInterface interface = {
        .recvPackage = JsonProtoProtoRecvPackage,
        .sendPackage = JsonProtoSendPackage,
        .deleteProto = DeleteJsonProto,
        .sendBranchPackages = JsonProtoSendBranchPackages
    };
    return interface;
}
