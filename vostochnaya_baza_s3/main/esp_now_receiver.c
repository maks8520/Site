 esp-idf-setup-10902462744309710451
#include <stdio.h>

#include "esp_now_receiver.h"
#include "esp_log.h"
#include "string.h"

static const char *TAG = "ESP_NOW";

QueueHandle_t esp_now_queue;

static void esp_now_recv_cb(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len) {
    if (len <= 0 || len >= 250) {
        ESP_LOGE(TAG, "Receive error, length: %d", len);
        return;
    }

    esp_now_event_t evt;
    memcpy(evt.mac_addr, recv_info->src_addr, 6);

    // Copy payload and ensure null termination
    memcpy(evt.payload, data, len);
    evt.payload[len] = '\0';

    // Simple filter to remove specific non-printable characters or perform sanitization if needed.
    // Assuming JSON format payload text
    for (int i = 0; i < len; i++) {
        if (evt.payload[i] < 32 || evt.payload[i] == '"' || evt.payload[i] == '\\') {
            evt.payload[i] = ' '; // Replace unprintable with space
        }
    }

    if (xQueueSend(esp_now_queue, &evt, portMAX_DELAY) != pdTRUE) {
        ESP_LOGW(TAG, "Queue full, dropping ESP-NOW packet");
    }
}

void esp_now_receiver_init(void) {
    esp_now_queue = xQueueCreate(10, sizeof(esp_now_event_t));
    if (esp_now_queue == NULL) {
        ESP_LOGE(TAG, "Create mutex fail");
        return;
    }

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(esp_now_recv_cb));

    ESP_LOGI(TAG, "ESP-NOW Init completed");
}
main
