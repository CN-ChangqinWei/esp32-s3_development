#include "communication.h"


#include <string.h>

Communication* NewCommunication(void* instance, CommInterface interface) {
    Communication* comm=(Communication*)pvPortMalloc(sizeof(Communication));
    if(comm == NULL) return NULL;
    comm->instance = instance;
    comm->interface = interface;
    comm->statu = MODE_LEN;
    comm->recvLen = 0;
    comm->totalLen = 0;
    comm->packageBuf = NULL;
    comm->bufCur = 0;
    return comm;
}

void DeleteCommunication(Communication* comm) {
    if(comm != NULL){
        if(comm->packageBuf != NULL){
            vPortFree(comm->packageBuf);
        }
        vPortFree(comm);
    }
}

void* CommRecvPackage(Communication* comm, int* len) {
    uint32_t tempLen;
    if (len != NULL) *len=0;
    if (comm->statu == MODE_LEN) {
        // 先接收4字节长度
        uint32_t recvBytes = comm->interface.recv(comm->instance, (uint8_t*)&tempLen, sizeof(uint32_t));
        if (recvBytes < sizeof(uint32_t)) {
            //CommSendPackage(comm,"recvBytesLenFail",strlen("recvBytesLenFail"));
            return NULL; // 长度未接收完整
        }
        //CommSendPackage(comm,(uint8_t*)&tempLen,sizeof(uint32_t));
        comm->recvLen=comm->totalLen = tempLen;
        
        // 使用FreeRTOS动态内存分配
        comm->packageBuf = (uint8_t*)pvPortMalloc(comm->recvLen);
        if (comm->packageBuf == NULL) {
            comm->recvLen = 0;
            comm->statu=MODE_LEN;
            //CommSendPackage(comm,(uint8_t*)"fail to malloc\n",strlen("fail to malloc\n"));
            return NULL; // 内存分配失败
        }
        
        // 切换到数据接收模式
        comm->statu = MODE_DATA;
        return NULL; // 长度接收完成，等待数据接收
        
    } else if (comm->statu == MODE_DATA) {
        // 接收数据写入内存（写入到当前偏移位置）
        uint32_t recvBytes = comm->interface.recv(comm->instance, comm->packageBuf + (comm->totalLen - comm->recvLen), comm->recvLen);
        comm->recvLen -= recvBytes;
        
        if(recvBytes!=0)
        
        
        // 接收完全后返回指针
        if (comm->recvLen == 0) {
            uint8_t* result = comm->packageBuf;
            // 重置状态，准备接收下一个包
            comm->statu = MODE_LEN;
            comm->recvLen = 0;
            comm->packageBuf = NULL;
            if (len != NULL) {
                *len = comm->totalLen;
            }
            return result;
        }
    }
    
    return NULL;
}

void CommSendPackage(Communication* comm, uint8_t* data, int len) {
    if (data == NULL || len <= 0) {
        return;
    }
    
    // 先发长度（4字节）
    uint32_t tempLen = (uint32_t)len;
    comm->interface.send(comm->instance, (uint8_t*)&tempLen, sizeof(uint32_t));
    
    // 再发数据
    comm->interface.send(comm->instance, data, len);
}

void CommHandler(Communication* comm){
    
}

