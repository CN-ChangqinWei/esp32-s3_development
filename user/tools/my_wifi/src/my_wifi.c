#include "my_wifi.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include <string.h>

static const char* TAG = "MY_WIFI";
static int wifi_connected = 0;

// 事件处理回调
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_connected = 0;
        ESP_LOGI(TAG, "WiFi disconnected, retrying...");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        wifi_connected = 1;
        ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
        ESP_LOGI(TAG, "WiFi connected, IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

// 初始化事件循环和WiFi
static int wifi_init_internal(void)
{
    static int initialized = 0;
    if (initialized) return 0;

    esp_err_t ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to create event loop");
        return -1;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init wifi");
        return -1;
    }

    ret = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register wifi event");
        return -1;
    }

    ret = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register ip event");
        return -1;
    }

    initialized = 1;
    return 0;
}

int WifiInit(const char* wifiSSD, const char* wifiPSWD)
{
    if (wifiSSD == NULL || wifiPSWD == NULL) {
        ESP_LOGE(TAG, "Invalid args");
        return -1;
    }

    if (wifi_init_internal() != 0) {
        return -1;
    }

    wifi_config_t wifi_config = {0};
    strncpy((char*)wifi_config.sta.ssid, wifiSSD, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char*)wifi_config.sta.password, wifiPSWD, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    esp_err_t ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set wifi mode");
        return -1;
    }

    ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set wifi config");
        return -1;
    }

    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start wifi");
        return -1;
    }

    ESP_LOGI(TAG, "WiFi init OK, connecting to %s...", wifiSSD);

    // 等待连接（最多10秒）
    int timeout = 100;
    while (!wifi_connected && timeout-- > 0) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    return wifi_connected ? 0 : -2;
}
