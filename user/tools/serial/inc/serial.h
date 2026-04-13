#ifndef _SERIAL_H
#define _SERIAL_H

#include "driver/uart.h"
#include "ring_buf.h"

#define _SERIAL_BUF_SIZE        255
#define _SERIAL_DMA_BUF_SIZE    1024

// ESP32 UART 配置结构体
typedef struct {
    uart_port_t uart_num;           // UART 端口号 (UART_NUM_0, UART_NUM_1, UART_NUM_2)
    uart_config_t uart_config;      // UART 配置
    int rx_buffer_size;             // 接收缓冲区大小
    int tx_buffer_size;             // 发送缓冲区大小
    int queue_size;                 // UART 事件队列大小
    QueueHandle_t* uart_queue;      // 事件队列句柄指针 (可为 NULL)
} SerialConfig;

typedef struct {
    uart_port_t uart_num;           // UART 端口号
    RingBuf recvRingBuf;            // 接收环形缓冲区
    uint8_t* sendBuf;               // 发送数组的指针
    uint32_t sendLen;               // 发送数组的长度
    uint32_t rxLen;
    uint32_t txLen;
    uint8_t recvFinishFlag;         // 接收完成标志
    uint8_t rxTmp;                  // 中断接收临时字节
    
    // ESP32 特有的 DMA/缓冲区相关
    uint8_t* bufDmaRX;              // DMA 接收缓冲区
    int wCurDmaRX;                  // DMA 写入指针
    int rCurDmaRX;                  // DMA 读取指针
    TaskHandle_t rxTaskHandle;      // 接收任务句柄
    uint8_t use_event_queue;        // 是否使用事件队列
} Serial;

extern Serial* serial1;
extern uint8_t sendBuf1[255];

// 创建/销毁 Serial 实例
Serial* NewSerial(const SerialConfig* config,
                  int recvBufSize,
                  uint8_t* sendBuf,
                  uint32_t sendLen);
void DeleteSerial(Serial* serial);

// 初始化所有串口（已迁移到 global.c 的 GlobalInit() 中）
// uint8_t SerialsInit(void);

// 中断接收方式
void SerialStartRecvIT(Serial* serial);
uint8_t SerialRecvIT(Serial* serial);

// DMA 接收方式 (ESP32 使用内部 DMA)
void SerialStartRecvDMA(Serial* serial);
uint8_t SerialRecvDMA(Serial* serial);

// 暂停接收 (阻塞方式接收指定长度)
uint8_t* SerialRecvPause(Serial* serial, uint8_t* buf, uint32_t len, uint32_t timeout);

// 处理函数 (需要在任务中调用或在中断回调中调用)
void SerialHandler(Serial* serial);
void SerialDmaHandler(Serial* serial);

// 缓冲区设置
uint8_t SerialSetRecvBuf(Serial* serial, int size);
uint8_t SerialSetSendBuf(Serial* serial, uint8_t* buf, uint32_t len);

// 发送/接收数据
uint32_t SerialSend(Serial* serial, uint32_t len);
uint32_t SerialRecv(Serial* serial);
uint32_t SerialSendUseOtherBuf(Serial* serial, uint8_t* buf, uint32_t len);
uint32_t SerialRecvUseOtherBuf(Serial* serial, uint8_t* buf, uint32_t len);

// 缓冲区操作
int SerialBufLen(Serial* serial);
uint32_t SerialReadBytes(Serial* serial, char* buf, int len);

// ESP32 特有：启动接收任务 (使用 FreeRTOS 任务处理接收)
uint8_t SerialStartRxTask(Serial* serial, UBaseType_t priority);

#endif
