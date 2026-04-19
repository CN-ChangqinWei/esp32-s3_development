#include "motor_serialize.h"
#include <stdlib.h>
#include"freertos/FreeRTOS.h"
#include"string.h"
static void* (*domainMalloc)(size_t size) = pvPortMalloc;
static void  (*domainFree)(void* p) =vPortFree;
void* MotorDomainReserialize(char* jsonStr) {
    MotorDomain* domain = pvPortMalloc(sizeof(MotorDomain));
    memset(domain,0,sizeof(MotorDomain));
    if (jsonStr == NULL) {
        return domain;
    }
    
    cJSON* root = cJSON_Parse(jsonStr);
    if (root == NULL) {
        return domain;
    }
    
    cJSON* item = cJSON_GetObjectItem(root, "protocol");
    if (cJSON_IsNumber(item)) {
        domain->protocol = (int)cJSON_GetNumberValue(item);
    }
    
    item = cJSON_GetObjectItem(root, "id");
    if (cJSON_IsNumber(item)) {
        domain->id = (int)cJSON_GetNumberValue(item);
    }
    
    item = cJSON_GetObjectItem(root, "powerOn");
    if (cJSON_IsNumber(item)) {
        domain->powerOn = (int)cJSON_GetNumberValue(item);
    }
    
    item = cJSON_GetObjectItem(root, "numAngel");
    if (cJSON_IsNumber(item)) {
        domain->numAngel = (uint32_t)cJSON_GetNumberValue(item);
    }
    
    item = cJSON_GetObjectItem(root, "denAngel");
    if (cJSON_IsNumber(item)) {
        domain->denAngel = (uint32_t)cJSON_GetNumberValue(item);
    }
    
    item = cJSON_GetObjectItem(root, "maxAngel");
    if (cJSON_IsNumber(item)) {
        domain->maxAngel = (uint32_t)cJSON_GetNumberValue(item);
    }
    
    item = cJSON_GetObjectItem(root, "encode");
    if (cJSON_IsNumber(item)) {
        domain->encode = (int)cJSON_GetNumberValue(item);
    }
    
    item = cJSON_GetObjectItem(root, "spEncode");
    if (cJSON_IsNumber(item)) {
        domain->spEncode = (int)cJSON_GetNumberValue(item);
    }
    
    item = cJSON_GetObjectItem(root, "pwmNum");
    if (cJSON_IsNumber(item)) {
        domain->pwmNum = (int)cJSON_GetNumberValue(item);
    }
    
    item = cJSON_GetObjectItem(root, "pwmDen");
    if (cJSON_IsNumber(item)) {
        domain->pwmDen = (int)cJSON_GetNumberValue(item);
    }
    
    item = cJSON_GetObjectItem(root, "spNumAngel");
    if (cJSON_IsNumber(item)) {
        domain->spNumAngel = (int)cJSON_GetNumberValue(item);
    }
    
    item = cJSON_GetObjectItem(root, "spDenAngel");
    if (cJSON_IsNumber(item)) {
        domain->spDenAngel = (int)cJSON_GetNumberValue(item);
    }
    
    item = cJSON_GetObjectItem(root, "mode");
    if (cJSON_IsNumber(item)) {
        domain->mode = (MotorMode)(int)cJSON_GetNumberValue(item);
    }
    
    cJSON_Delete(root);
    return domain;
}

char* MotorDomainSerialize(void* data) {
    if (data == NULL) {
        return NULL;
    }
    MotorDomain* domain=data;
    cJSON* root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }
    
    cJSON_AddNumberToObject(root, "protocol", domain->protocol);
    cJSON_AddNumberToObject(root, "id", domain->id);
    cJSON_AddNumberToObject(root, "powerOn", domain->powerOn);
    cJSON_AddNumberToObject(root, "numAngel", domain->numAngel);
    cJSON_AddNumberToObject(root, "denAngel", domain->denAngel);
    cJSON_AddNumberToObject(root, "maxAngel", domain->maxAngel);
    cJSON_AddNumberToObject(root, "encode", domain->encode);
    cJSON_AddNumberToObject(root, "spEncode", domain->spEncode);
    cJSON_AddNumberToObject(root, "pwmNum", domain->pwmNum);
    cJSON_AddNumberToObject(root, "pwmDen", domain->pwmDen);
    cJSON_AddNumberToObject(root, "spNumAngel", domain->spNumAngel);
    cJSON_AddNumberToObject(root, "spDenAngel", domain->spDenAngel);
    cJSON_AddNumberToObject(root, "mode", domain->mode);
    
    char* jsonStr = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    
    return jsonStr;
}
