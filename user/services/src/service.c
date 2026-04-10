#include "service.h"
#include "communication.h"
#include "router.h"
#include "health_comm.h"
#include "serial.h"
#include "motor_repo.h"
#include "motor_service.h"
static Service service={0};
static SerialComm* serialComm=NULL;
void SerivceInit(){
    
    SerialsInit();
    serialComm=NewSerialComm(serial1);
    service.listener=NewCommunicationFromSerial(serialComm);
    if(service.listener != NULL){
        //CommSendPackage(service.listener,"hello",strlen("hello"));
    }
    RouterInit();
    RouterHandlerPkg healthHandler = {HealthCommHandler,NULL};
    RouterRegister(Health, healthHandler);
    RouterHandlerPkg errHandler ={ServiceErrHandler,&service};
    RouterSetErrHandler(errHandler);
}

void ServiceExec(){
    RouterExec();
}
void ServiceCommHanlder(){
    static int cnt=0;
    if(service.listener == NULL) return;
    // cnt++;
    // if(cnt>=50){
    //     cnt=0;
    //     CommSendPackage(service.listener,"runing\n",strlen("runing\n"));
    // }
    int len=0;
    char* buf=CommRecvPackage(service.listener,&len);
    if(NULL==buf) return;
    RouterAnlyPackage(buf,len);
    //CommSendPackage(service.listener,buf,len);
}
void ServiceComm(char* buf,int len){
    if(service.listener == NULL) return;
    CommSendPackage(service.listener,(uint8_t*)buf,len);
}
uint16_t ServiceErrHandler(void*instance,void* arg){
    char msg[60]={0};
    sprintf(msg,"errhandler:%s",(char*)arg);
    CommSendPackage(service.listener,(uint8_t*)msg, strlen(msg));
    return 0;
}