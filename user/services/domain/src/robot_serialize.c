#include "robot_serialize.h"
#include "freertos/FreeRTOS.h"
#include <string.h>

void* RobotDomainDeserialize(char* jsonStr) {
    if (jsonStr == NULL) return NULL;
    
    cJSON* root = cJSON_Parse(jsonStr);
    if (root == NULL) return NULL;
    
    RobotDomain* domain = (RobotDomain*)pvPortMalloc(sizeof(RobotDomain));
    if (domain == NULL) {
        cJSON_Delete(root);
        return NULL;
    }
    
    cJSON* item = cJSON_GetObjectItem(root, "protocol");
    if (item != NULL) {
        domain->protocol = item->valueint;
    }
    
    item = cJSON_GetObjectItem(root, "x");
    if (item != NULL) {
        domain->x = (AxisFloat)item->valuedouble;
    }
    
    item = cJSON_GetObjectItem(root, "y");
    if (item != NULL) {
        domain->y = (AxisFloat)item->valuedouble;
    }
    
    item = cJSON_GetObjectItem(root, "z");
    if (item != NULL) {
        domain->z = (AxisFloat)item->valuedouble;
    }
    
    cJSON_Delete(root);
    return domain;
}

char* RobotDomainSerialize(void* domain) {
    if (domain == NULL) return NULL;
    
    RobotDomain* d = (RobotDomain*)domain;
    cJSON* root = cJSON_CreateObject();
    if (root == NULL) return NULL;
    
    cJSON_AddNumberToObject(root, "protocol", d->protocol);
    cJSON_AddNumberToObject(root, "res", ((RobotDomainReply*)d)->res);
    
    char* str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return str;
}
