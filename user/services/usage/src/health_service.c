#include"health_service.h"
#include <string.h>

void * HealthExec(int* len){
    *len=strlen("health ok");
    return "health ok";
}