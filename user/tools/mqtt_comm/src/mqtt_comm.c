#include "mqtt_comm.h"
#include "esp_log.h"
#include <string.h>

static const char* TAG = "MQTT_COMM";

// 内部函数声明
static void MqttEventHandler(void* handler_args, esp_event_base_t base, int32_t event_id, void* event_data);
static int AddRecvData(MqttComm* mqttComm, const char* data, uint32_t len);
static void FreeRecvNode(MqttRecvNode* node);

// 自动修正 URI 格式，确保有 mqtt:// 前缀
static void fix_mqtt_uri(const char* input, char* output, size_t output_size) {
    if (input == NULL || output == NULL || output_size == 0) return;
    
    // 检查是否已有协议前缀
    if (strncmp(input, "mqtt://", 7) == 0 || strncmp(input, "mqtts://", 8) == 0 ||
        strncmp(input, "ws://", 5) == 0 || strncmp(input, "wss://", 6) == 0) {
        strncpy(output, input, output_size - 1);
        output[output_size - 1] = '\0';
    } else {
        // 自动添加 mqtt:// 前缀
        snprintf(output, output_size, "mqtt://%s", input);
    }
}

// 创建 MqttComm 对象
Communication* NewMqttComm(MqttConfig* config) {
    if (config == NULL) {
        ESP_LOGE(TAG, "Config is NULL");
        return NULL;
    }

    MqttComm* mqttComm = (MqttComm*)CommMalloc(sizeof(MqttComm));
    if (mqttComm == NULL) {
        ESP_LOGE(TAG, "Failed to allocate MqttComm");
        return NULL;
    }
    memset(mqttComm, 0, sizeof(MqttComm));

    // 复制配置
    memcpy(&mqttComm->config, config, sizeof(MqttConfig));
    
    // 修正 URI 格式
    char fixed_uri[128];
    fix_mqtt_uri(mqttComm->config.uri, fixed_uri, sizeof(fixed_uri));
    strncpy(mqttComm->config.uri, fixed_uri, sizeof(mqttComm->config.uri) - 1);
    mqttComm->config.uri[sizeof(mqttComm->config.uri) - 1] = '\0';
    
    // 创建互斥锁
    mqttComm->recvMutex = xSemaphoreCreateMutex();
    if (mqttComm->recvMutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        CommFree(mqttComm);
        return NULL;
    }

    // 配置 MQTT 客户端
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = mqttComm->config.uri,
        .credentials.client_id = mqttComm->config.client_id[0] ? mqttComm->config.client_id : NULL,
        .credentials.username = mqttComm->config.username[0] ? mqttComm->config.username : NULL,
        .credentials.authentication.password = mqttComm->config.password[0] ? mqttComm->config.password : NULL,
        .session.keepalive = mqttComm->config.keepalive > 0 ? mqttComm->config.keepalive : 120,
    };

    mqttComm->client = esp_mqtt_client_init(&mqtt_cfg);
    if (mqttComm->client == NULL) {
        ESP_LOGE(TAG, "Failed to init MQTT client");
        vSemaphoreDelete(mqttComm->recvMutex);
        CommFree(mqttComm);
        return NULL;
    }

    // 注册事件处理
    esp_mqtt_client_register_event(mqttComm->client, ESP_EVENT_ANY_ID, MqttEventHandler, mqttComm);

    // 启动客户端
    esp_err_t err = esp_mqtt_client_start(mqttComm->client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start MQTT client: %d", err);
        esp_mqtt_client_destroy(mqttComm->client);
        vSemaphoreDelete(mqttComm->recvMutex);
        CommFree(mqttComm);
        return NULL;
    }

    ESP_LOGI(TAG, "MQTT client started, connecting to %s", mqttComm->config.uri);
    return NewCommunication(mqttComm, GetMqttCommInterface());
}

// 销毁 MqttComm 对象
void DeleteMqttComm(void* instance) {
    if (instance == NULL) return;

    MqttComm* mqttComm = (MqttComm*)instance;

    // 停止并销毁客户端
    if (mqttComm->client != NULL) {
        esp_mqtt_client_stop(mqttComm->client);
        esp_mqtt_client_destroy(mqttComm->client);
    }

    // 清空接收缓冲区
    if (mqttComm->recvMutex != NULL) {
        xSemaphoreTake(mqttComm->recvMutex, portMAX_DELAY);
    }
    
    MqttRecvNode* node = mqttComm->recvHead;
    while (node != NULL) {
        MqttRecvNode* next = node->next;
        FreeRecvNode(node);
        node = next;
    }
    
    if (mqttComm->curRecvNode != NULL) {
        FreeRecvNode(mqttComm->curRecvNode);
    }

    if (mqttComm->recvMutex != NULL) {
        xSemaphoreGive(mqttComm->recvMutex);
        vSemaphoreDelete(mqttComm->recvMutex);
    }

    CommFree(mqttComm);
    ESP_LOGI(TAG, "MqttComm destroyed");
}

// 发送数据（发布到默认主题）
uint32_t MqttCommSend(void* instance, char* data, uint32_t len) {
    if (instance == NULL || data == NULL || len == 0) return 0;

    MqttComm* mqttComm = (MqttComm*)instance;
    if (!mqttComm->connected || mqttComm->client == NULL) return 0;

    int msg_id = esp_mqtt_client_publish(
        mqttComm->client,
        mqttComm->config.pub_topic,
        data,
        len,
        mqttComm->config.qos,
        0  // retain = false
    );

    if (msg_id < 0) {
        ESP_LOGE(TAG, "Publish failed");
        return 0;
    }

    return len;
}

// 接收数据（从缓冲区读取）
uint32_t MqttCommRecv(void* instance, char* data, uint32_t len) {
    if (instance == NULL || data == NULL || len == 0) return 0;

    MqttComm* mqttComm = (MqttComm*)instance;
    
    if (mqttComm->recvMutex == NULL) return 0;

    if (xSemaphoreTake(mqttComm->recvMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return 0;
    }

    uint32_t totalRead = 0;

    // 首先处理当前未完成的节点
    if (mqttComm->curRecvNode != NULL) {
        MqttRecvNode* node = mqttComm->curRecvNode;
        uint32_t remaining = node->len - mqttComm->curRecvOffset;
        uint32_t toRead = (remaining < len) ? remaining : len;
        
        memcpy(data, node->data + mqttComm->curRecvOffset, toRead);
        totalRead += toRead;
        mqttComm->curRecvOffset += toRead;
        
        if (mqttComm->curRecvOffset >= node->len) {
            FreeRecvNode(mqttComm->curRecvNode);
            mqttComm->curRecvNode = NULL;
            mqttComm->curRecvOffset = 0;
        }
    }

    // 继续从链表头部读取
    while (totalRead < len && mqttComm->recvHead != NULL) {
        MqttRecvNode* node = mqttComm->recvHead;
        uint32_t need = len - totalRead;
        uint32_t toRead = (node->len < need) ? node->len : need;
        
        memcpy(data + totalRead, node->data, toRead);
        totalRead += toRead;
        mqttComm->recvTotalLen -= toRead;
        
        if (toRead < node->len) {
            // 数据未读完，保存到 curRecvNode
            mqttComm->curRecvNode = node;
            mqttComm->curRecvOffset = toRead;
            mqttComm->recvHead = node->next;
            if (mqttComm->recvTail == node) {
                mqttComm->recvTail = NULL;
            }
            break;
        } else {
            // 节点数据读完，释放
            mqttComm->recvHead = node->next;
            if (mqttComm->recvTail == node) {
                mqttComm->recvTail = NULL;
            }
            FreeRecvNode(node);
        }
    }

    xSemaphoreGive(mqttComm->recvMutex);
    return totalRead;
}

// 获取接口
CommInterface GetMqttCommInterface(void) {
    CommInterface interface = {
        .send = MqttCommSend,
        .recv = MqttCommRecv,
        .deleteInstance = DeleteMqttComm
    };
    return interface;
}

// 便捷函数
Communication* NewCommunicationFromMqttComm(MqttComm* instance) {
    if (instance == NULL) return NULL;
    return NewCommunication(instance, GetMqttCommInterface());
}

// 检查连接状态
int MqttCommIsConnected(void* instance) {
    if (instance == NULL) return 0;
    MqttComm* mqttComm = (MqttComm*)instance;
    return mqttComm->connected;
}

// 订阅主题
int MqttCommSubscribe(void* instance, const char* topic, int qos) {
    if (instance == NULL || topic == NULL) return -1;
    MqttComm* mqttComm = (MqttComm*)instance;
    if (!mqttComm->connected || mqttComm->client == NULL) return -1;

    int msg_id = esp_mqtt_client_subscribe(mqttComm->client, topic, qos);
    if (msg_id < 0) {
        ESP_LOGE(TAG, "Subscribe failed: %s", topic);
        return -1;
    }
    ESP_LOGI(TAG, "Subscribed to %s, msg_id=%d", topic, msg_id);
    return msg_id;
}

// 发布到指定主题
int MqttCommPublish(void* instance, const char* topic, char* data, uint32_t len, int qos) {
    if (instance == NULL || topic == NULL || data == NULL || len == 0) return -1;
    MqttComm* mqttComm = (MqttComm*)instance;
    if (!mqttComm->connected || mqttComm->client == NULL) return -1;

    int msg_id = esp_mqtt_client_publish(mqttComm->client, topic, data, len, qos, 0);
    if (msg_id < 0) {
        ESP_LOGE(TAG, "Publish to %s failed", topic);
        return -1;
    }
    return msg_id;
}

// MQTT 事件处理
static void MqttEventHandler(void* handler_args, esp_event_base_t base, int32_t event_id, void* event_data) {
    MqttComm* mqttComm = (MqttComm*)handler_args;
    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
            mqttComm->connected = 1;
            // 自动订阅默认主题
            if (mqttComm->config.sub_topic[0]) {
                esp_mqtt_client_subscribe(mqttComm->client, mqttComm->config.sub_topic, mqttComm->config.qos);
            }
            break;

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
            mqttComm->connected = 0;
            break;

        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
            break;

        case MQTT_EVENT_UNSUBSCRIBED:
            ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
            break;

        case MQTT_EVENT_PUBLISHED:
            ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
            break;

        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "MQTT_EVENT_DATA, topic=%.*s, len=%d", 
                     event->topic_len, event->topic, event->data_len);
            // 将接收到的数据加入缓冲区
            AddRecvData(mqttComm, event->data, event->data_len);
            break;

        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT_EVENT_ERROR");
            break;

        default:
            break;
    }
}

// 添加接收数据到缓冲区
static int AddRecvData(MqttComm* mqttComm, const char* data, uint32_t len) {
    if (mqttComm == NULL || data == NULL || len == 0) return -1;

    MqttRecvNode* node = (MqttRecvNode*)CommMalloc(sizeof(MqttRecvNode));
    if (node == NULL) return -1;

    node->data = (char*)CommMalloc(len);
    if (node->data == NULL) {
        CommFree(node);
        return -1;
    }

    memcpy(node->data, data, len);
    node->len = len;
    node->next = NULL;

    if (xSemaphoreTake(mqttComm->recvMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        FreeRecvNode(node);
        return -1;
    }

    // 添加到链表尾部
    if (mqttComm->recvTail == NULL) {
        mqttComm->recvHead = node;
        mqttComm->recvTail = node;
    } else {
        mqttComm->recvTail->next = node;
        mqttComm->recvTail = node;
    }
    mqttComm->recvTotalLen += len;

    xSemaphoreGive(mqttComm->recvMutex);
    return 0;
}

// 释放接收节点
static void FreeRecvNode(MqttRecvNode* node) {
    if (node == NULL) return;
    if (node->data != NULL) {
        CommFree(node->data);
    }
    CommFree(node);
}
