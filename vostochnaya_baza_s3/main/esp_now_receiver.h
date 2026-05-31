#pragma once

#include "esp_now.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

// Define queue for parsed data
extern QueueHandle_t esp_now_queue;

typedef struct {
    uint8_t mac_addr[6];
    char payload[250];
} esp_now_event_t;

void esp_now_receiver_init(void);

#ifdef __cplusplus
}
#endif
