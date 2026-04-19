#include "serial.h"
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/uart.h"
#include "esp_log.h"

static const char* TAG = "SERIAL";

// 接收任务函数
static void SerialRxTask(void* pvParameters) {
    Serial* serial = (Serial*)pvParameters;
    char* tempBuf = pvPortMalloc(_SERIAL_DMA_BUF_SIZE);
    if (tempBuf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate temp buffer for RX task");
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        // 读取数据到临时缓冲区
        int len = uart_read_bytes(serial->uart_num, tempBuf, _SERIAL_DMA_BUF_SIZE, pdMS_TO_TICKS(100));
        if (len > 0) {
            // 将数据存入环形缓冲区
            RingBufAddData(&serial->recvRingBuf, (char*)tempBuf, len);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

Serial* NewSerial(const SerialConfig* config,
                  int recvBufSize,
                  char* sendBuf,
                  uint32_t sendLen) {
    if (config == NULL) return NULL;

    Serial* serial = (Serial*)pvPortMalloc(sizeof(Serial));
    if (serial == NULL) return NULL;

    memset(serial, 0, sizeof(Serial));

    serial->uart_num = config->uart_num;
    serial->recvRingBuf = NewRingBuf(recvBufSize);
    serial->sendBuf = sendBuf;
    serial->sendLen = sendLen;

    // 配置 UART
    esp_err_t err = uart_param_config(config->uart_num, &config->uart_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART param config failed");
        vPortFree(serial);
        return NULL;
    }

    // 安装 UART 驱动 (启用内部 DMA)
    int rx_buf_size = config->rx_buffer_size > 0 ? config->rx_buffer_size : _SERIAL_DMA_BUF_SIZE;
    int tx_buf_size = config->tx_buffer_size > 0 ? config->tx_buffer_size : 0;

    err = uart_driver_install(config->uart_num, rx_buf_size, tx_buf_size,
                              config->queue_size, config->uart_queue, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART driver install failed");
        vPortFree(serial);
        return NULL;
    }

    // 分配 DMA 缓冲区用于兼容层
    serial->bufDmaRX = pvPortMalloc(_SERIAL_DMA_BUF_SIZE);
    if (serial->bufDmaRX == NULL) {
        ESP_LOGW(TAG, "Failed to allocate DMA RX buffer");
    }

    serial->use_event_queue = (config->uart_queue != NULL && config->queue_size > 0) ? 1 : 0;

    return serial;
}

void DeleteSerial(Serial* serial) {
    if (serial == NULL) return;

    // 停止接收任务
    if (serial->rxTaskHandle != NULL) {
        vTaskDelete(serial->rxTaskHandle);
        serial->rxTaskHandle = NULL;
    }

    // 卸载 UART 驱动
    uart_driver_delete(serial->uart_num);

    // 释放缓冲区
    if (serial->recvRingBuf.buffer != NULL) {
        vPortFree(serial->recvRingBuf.buffer);
    }
    if (serial->bufDmaRX != NULL) {
        vPortFree(serial->bufDmaRX);
    }
    vPortFree(serial);
}

// SerialsInit 已迁移到 global.c 的 GlobalInit() 中

char SerialStartRxTask(Serial* serial, UBaseType_t priority) {
    if (serial == NULL) return 1;

    BaseType_t ret = xTaskCreate(SerialRxTask, "serial_rx_task", 2048,
                                  serial, priority, &serial->rxTaskHandle);
    return (ret == pdPASS) ? 0 : 1;
}

// 中断接收方式：ESP32 使用轮询任务替代
void SerialStartRecvIT(Serial* serial) {
    // ESP32 使用任务方式接收，此方法保持兼容性
    if (serial != NULL && serial->rxTaskHandle == NULL) {
        SerialStartRxTask(serial, 5);
    }
}

char SerialRecvIT(Serial* serial) {
    // 从 UART 读取一个字节并存入环形缓冲区
    if (serial == NULL) return 1;

    char byte;
    int len = uart_read_bytes(serial->uart_num, &byte, 1, 0);
    if (len > 0) {
        RingBufAddByte(&serial->recvRingBuf, (char)byte);
    }
    return 0;
}

// DMA 接收方式：ESP32 内部已使用 DMA，这里提供兼容层
void SerialStartRecvDMA(Serial* serial) {
    // ESP32 UART 驱动内部使用 DMA，此函数仅重置指针
    if (serial != NULL) {
        serial->wCurDmaRX = 0;
        serial->rCurDmaRX = 0;
    }
}

char SerialRecvDMA(Serial* serial) {
    // ESP32 UART 驱动会自动将数据存入内部缓冲区
    // 这里从内部缓冲区读取并存入 ringbuf
    if (serial == NULL) return 1;

    if (serial->bufDmaRX == NULL) return 1;

    int len = uart_read_bytes(serial->uart_num, serial->bufDmaRX, _SERIAL_DMA_BUF_SIZE, 0);
    if (len > 0) {
        RingBufAddData(&serial->recvRingBuf, (char*)serial->bufDmaRX, len);
        serial->wCurDmaRX += len;
        if (serial->wCurDmaRX >= _SERIAL_DMA_BUF_SIZE) {
            serial->wCurDmaRX = 0;
        }
    }
    return 0;
}

char* SerialRecvPause(Serial* serial, char* buf, uint32_t len, uint32_t timeout) {
    if (serial == NULL || buf == NULL || len == 0) return NULL;

    // 使用阻塞方式读取
    int rx_len = uart_read_bytes(serial->uart_num, buf, len, pdMS_TO_TICKS(timeout));
    if (rx_len > 0) {
        serial->rxLen = rx_len;
        serial->recvFinishFlag = 1;
        // 同时存入环形缓冲区
        RingBufAddData(&serial->recvRingBuf, (char*)buf, rx_len);
        return buf;
    }

    return NULL;
}

void SerialHandler(Serial* serial) {
    if (serial == NULL) return;
    // 轮询方式接收一个字节
    SerialRecvIT(serial);
}

void SerialDmaHandler(Serial* serial) {
    if (serial == NULL) return;
    // 处理 DMA 接收
    SerialRecvDMA(serial);
}

char SerialSetRecvBuf(Serial* serial, int size) {
    if (serial == NULL || size <= 0) return 1;

    if (serial->recvRingBuf.buffer != NULL) {
        vPortFree(serial->recvRingBuf.buffer);
    }
    serial->recvRingBuf = NewRingBuf(size);
    return (serial->recvRingBuf.buffer != NULL) ? 0 : 1;
}

char SerialSetSendBuf(Serial* serial, char* buf, uint32_t len) {
    if (serial == NULL || buf == NULL) return 1;
    serial->sendBuf = buf;
    serial->sendLen = len;
    return 0;
}

uint32_t SerialSend(Serial* serial, uint32_t len) {
    if (serial == NULL || serial->sendBuf == NULL || len == 0) return 0;
    if (len > serial->sendLen) len = serial->sendLen;

    int sent = uart_write_bytes(serial->uart_num, (const char*)serial->sendBuf, len);
    if (sent > 0) {
        serial->txLen += sent;
    }
    return (sent > 0) ? (uint32_t)sent : 0;
}

uint32_t SerialRecv(Serial* serial) {
    if (serial == NULL) return 0;

    char tempBuf[_SERIAL_BUF_SIZE];
    int len = uart_read_bytes(serial->uart_num, tempBuf, _SERIAL_BUF_SIZE, 0);
    if (len > 0) {
        RingBufAddData(&serial->recvRingBuf, (char*)tempBuf, len);
        return (uint32_t)len;
    }
    return 0;
}

uint32_t SerialSendUseOtherBuf(Serial* serial, char* buf, uint32_t len) {
    if (serial == NULL || buf == NULL || len == 0) return 0;

    if (buf == serial->sendBuf) {
        return SerialSend(serial, len);
    }

    int sent = uart_write_bytes(serial->uart_num, (const char*)buf, len);
    if (sent > 0) {
        serial->txLen += sent;
    }
    return (sent > 0) ? (uint32_t)sent : 0;
}

uint32_t SerialRecvUseOtherBuf(Serial* serial, char* buf, uint32_t len) {
    if (serial == NULL || buf == NULL || len == 0) return 0;

    int rx_len = uart_read_bytes(serial->uart_num, buf, len, 0);
    if (rx_len > 0) {
        RingBufAddData(&serial->recvRingBuf, (char*)buf, rx_len);
        return (uint32_t)rx_len;
    }
    return 0;
}

int SerialBufLen(Serial* serial) {
    if (serial == NULL || serial->recvRingBuf.buffer == NULL) return 0;
    return serial->recvRingBuf.len;
}

uint32_t SerialReadBytes(Serial* serial, char* buf, int len) {
    if (serial == NULL || buf == NULL || len <= 0 || serial->recvRingBuf.buffer == NULL) return 0;
    return RingBufRead(&serial->recvRingBuf, buf, len);
}
