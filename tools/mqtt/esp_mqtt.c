
#include "esp_mqtt.h"
static const char *TAG = "ESP_MQTT";

static esp_err_t mqtt_event_handler_cb(esp_mqtt_event_handle_t event)
{
    MQTT *mqtt = (MQTT *)event->user_context;
    
    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
            if (mqtt->config.username[0] != '\0') {
                esp_mqtt_client_subscribe(mqtt->client, mqtt->config.username, 0);
            }
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
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
            ESP_LOGI(TAG, "MQTT_EVENT_DATA");
            if (mqtt != NULL && mqtt->recvBuf != NULL) {
                RingBufAddData(mqtt->recvBuf, event->data, event->data_len);
            }
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
            break;
        default:
            ESP_LOGI(TAG, "Other event id:%d", event->event_id);
            break;
    }
    return ESP_OK;
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%" PRIi32 "", base, event_id);
    mqtt_event_handler_cb(event_data);
}

int MqttRecv(MQTT* mqtt, char* buf, int len)
{
    if (mqtt == NULL || mqtt->recvBuf == NULL || buf == NULL || len <= 0) {
        return 0;
    }
    return RingBufRead(mqtt->recvBuf, buf, len);
}

int MqttSend(MQTT* mqtt, char* buf, int len)
{
    if (mqtt == NULL || mqtt->client == NULL || buf == NULL || len <= 0) {
        return 0;
    }
    return esp_mqtt_client_publish(mqtt->client, mqtt->config.username, buf, len, 0, 0);
}

void MqttDelete(MQTT** mqtt)
{
    if (mqtt == NULL || *mqtt == NULL) {
        return;
    }
    
    MQTT* m = *mqtt;
    
    MqttDisconnect(m);
    
    if (m->client != NULL) {
        esp_mqtt_client_destroy(m->client);
        m->client = NULL;
    }
    
    if (m->recvBuf != NULL) {
        if (m->recvBuf->buffer != NULL) {
            vPortFree(m->recvBuf->buffer);
        }
        vPortFree(m->recvBuf);
    }
    
    vPortFree(m);
    *mqtt = NULL;
}

MQTT* NewMqtt(MQTTConfig config, int bufSize)
{
    MQTT* mqtt = (MQTT*)_MQTT_MALLOC(sizeof(MQTT));
    if (mqtt == NULL) {
        ESP_LOGE(TAG, "Failed to allocate MQTT object");
        return NULL;
    }
    
    memset(mqtt, 0, sizeof(MQTT));
    memcpy(&mqtt->config, &config, sizeof(MQTTConfig));
    
    mqtt->recvBuf = (RingBuf*)_MQTT_MALLOC(sizeof(RingBuf));
    if (mqtt->recvBuf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate ring buffer");
        vPortFree(mqtt);
        return NULL;
    }
    
    *mqtt->recvBuf = NewRingBuf(bufSize);
    if (mqtt->recvBuf->buffer == NULL) {
        ESP_LOGE(TAG, "Failed to create ring buffer");
        vPortFree(mqtt->recvBuf);
        vPortFree(mqtt);
        return NULL;
    }
    
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = mqtt->config.url,
        .broker.address.port = 1883,
        .credentials.client_id = mqtt->config.cliendId,
        .credentials.username = mqtt->config.username,
        .credentials.authentication.password = mqtt->config.pswd,
        .session.keepalive = 120,
        .buffer.size = 1024,
        .buffer.out_size = 1024
    };
    
    mqtt->client = esp_mqtt_client_init(&mqtt_cfg);
    if (mqtt->client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize MQTT client");
        if (mqtt->recvBuf->buffer != NULL) {
            vPortFree(mqtt->recvBuf->buffer);
        }
        vPortFree(mqtt->recvBuf);
        vPortFree(mqtt);
        return NULL;
    }
    
    esp_mqtt_client_register_event(mqtt->client, ESP_EVENT_ANY_ID, mqtt_event_handler, mqtt);
    
    esp_err_t err = esp_mqtt_client_start(mqtt->client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start MQTT client: %d", err);
        esp_mqtt_client_destroy(mqtt->client);
        if (mqtt->recvBuf->buffer != NULL) {
            vPortFree(mqtt->recvBuf->buffer);
        }
        vPortFree(mqtt->recvBuf);
        vPortFree(mqtt);
        return NULL;
    }
    
    return mqtt;
}

int MqttDisconnect(MQTT* mqtt)
{
    if (mqtt == NULL || mqtt->client == NULL) {
        return -1;
    }
    esp_err_t err = esp_mqtt_client_stop(mqtt->client);
    if (err != ESP_OK) {
        return -1;
    }
    return 0;
}

int MqttConnect(MQTT* mqtt)
{
    if (mqtt == NULL || mqtt->client == NULL) {
        return -1;
    }
    esp_err_t err = esp_mqtt_client_start(mqtt->client);
    if (err != ESP_OK) {
        return -1;
    }
    return 0;
}
