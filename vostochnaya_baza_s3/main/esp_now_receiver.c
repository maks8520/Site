#include "esp_now_receiver.h"
#include "esp_now.h"
#include "esp_log.h"
#include "esp_mac.h"
#include <string.h>

static const char *TAG = "ESP_NOW_RECV";

static void esp_now_recv_cb(const esp_now_recv_info_t *esp_now_info, const uint8_t *data, int len) {
    ESP_LOGI(TAG, "Received %d bytes from " MACSTR, len, MAC2STR(esp_now_info->src_addr));
}

void esp_now_receiver_init(void) {
    if (esp_now_init() != ESP_OK) {
        ESP_LOGE(TAG, "Error initializing ESP-NOW");
        return;
    }
    esp_now_register_recv_cb(esp_now_recv_cb);
    ESP_LOGI(TAG, "ESP-NOW receiver initialized");
}
