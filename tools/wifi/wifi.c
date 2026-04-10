#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "wifi.h"

static const char *TAG = "WIFI";
static EventGroupHandle_t s_wifi_event_group;
static esp_netif_t *sta_netif = NULL;

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define WIFI_MAX_RETRY  5

int WifiConnect(char* wifiName, char* pswd) {
    // if (wifiName == NULL || strlen(wifiName) == 0) {
    //     ESP_LOGE(TAG, "WiFi name is empty");
    //     return -1;
    // }

    // s_wifi_event_group = xEventGroupCreate();

    // ESP_ERROR_CHECK(esp_netif_init());
    // ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    // if (sta_netif == NULL) {
    //     sta_netif = esp_netif_create_default_wifi_sta();
    // }

    // wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    // ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
    //                                                     ESP_EVENT_ANY_ID,
    //                                                     &wifi_event_handler,
    //                                                     NULL,
    //                                                     NULL));
    // ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
    //                                                     IP_EVENT_STA_GOT_IP,
    //                                                     &wifi_event_handler,
    //                                                     NULL,
    //                                                     NULL));

    // wifi_config_t wifi_config = {
    //     .sta = {
    //         .ssid = {0},
    //         .password = {0},
    //         .threshold.authmode = WIFI_AUTH_WPA2_PSK,
    //         .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
    //     },
    // };
    // strncpy((char*)wifi_config.sta.ssid, wifiName, sizeof(wifi_config.sta.ssid) - 1);
    // if (pswd != NULL) {
    //     strncpy((char*)wifi_config.sta.password, pswd, sizeof(wifi_config.sta.password) - 1);
    // }

    // ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    // ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    // ESP_ERROR_CHECK(esp_wifi_start());

    // ESP_LOGI(TAG, "Connecting to WiFi SSID: %s", wifiName);

    // EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
    //                                        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
    //                                        pdFALSE,
    //                                        pdFALSE,
    //                                        pdMS_TO_TICKS(10000));

    // if (bits & WIFI_CONNECTED_BIT) {
    //     ESP_LOGI(TAG, "Connected to WiFi");
    //     return 0;
    // } else {
    //     ESP_LOGE(TAG, "Failed to connect to WiFi");
    //     return -1;
    // }
   
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = wifiName,
            .password = pswd,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to AP SSID:%s", WIFI_SSID);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s", WIFI_SSID);
    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }

}

int WifiDisconnect() {
    ESP_LOGI(TAG, "Disconnecting WiFi...");
    esp_err_t err = esp_wifi_disconnect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to disconnect WiFi: %d", err);
        return -1;
    }
    
    err = esp_wifi_stop();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to stop WiFi: %d", err);
        return -1;
    }
    
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    ESP_LOGI(TAG, "WiFi disconnected");
    return 0;
}
