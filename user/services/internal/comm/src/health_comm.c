#include"health_comm.h"
#include "health_service.h"
#include"service.h"
uint16_t HealthCommHandler(void* instance,void* arg){
    Service* service = (Service*)instance;
    int len;
    char* res=HealthExec(&len);
    ServiceComm(service,res,len);
    return 0;
}
