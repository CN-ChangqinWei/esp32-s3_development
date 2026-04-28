#include "serial_comm.h"
#include "serial.h"
#include "esp_log.h"
static const char TAG[]="SERIAL_COMM";

Communication* NewSerialComm(Serial* serial) {
    SerialComm* serialComm=(SerialComm*)CommMalloc(sizeof(SerialComm));
    if(serialComm == NULL) return NULL;
    serialComm->serial = serial;
    serialComm->tempRecvBuf = NULL;
    serialComm->tempRecvLen = 0;
    serialComm->tempRecvCur = 0;
    //SerialStartRecvIT(serial);
    return NewCommunication(serialComm,GetSerialCommInterface());
}

void DeleteSerialComm(void* serialComm) {
    if (serialComm != NULL) {
        SerialComm* p=serialComm;
        CommFree(p);
    }
}

uint32_t SerialCommSend(void* instance, char* data, uint32_t len) {
    if (instance == NULL || data == NULL || len == 0) return 0;
    
    SerialComm* serialComm = (SerialComm*)instance;
    if (serialComm->serial == NULL) return 0;
    int sendLen=SerialSendUseOtherBuf(serialComm->serial, data, len);
    ESP_LOGE(TAG, "Send package len = %d",sendLen);
    // 使用 Serial 的发送函数发送数据
    return sendLen;
}

uint32_t SerialCommRecv(void* instance, char* data, uint32_t len) {
    if (instance == NULL || data == NULL || len == 0) return 0;
    
    SerialComm* serialComm = (SerialComm*)instance;
    if (serialComm->serial == NULL) return 0;
    
    // 使用 Serial 模块的阻塞接收函数
    if(SerialBufLen(serialComm->serial)<len) return 0;
    uint32_t readLen = SerialReadBytes(serialComm->serial, (char*)data, len);
    ESP_LOGE(TAG, "Recv data len = %d",readLen);
    return readLen;
}

CommInterface GetSerialCommInterface(void) {
    CommInterface interface;
    interface.send = SerialCommSend;
    interface.recv = SerialCommRecv;
    interface.deleteInstance = DeleteSerialComm;
    return interface;
}

Communication* NewCommunicationFromSerial(SerialComm* instance) {
    if (instance == NULL) return NULL;
    return NewCommunication(instance, GetSerialCommInterface());
}
