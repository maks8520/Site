#include "gps_parser.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "GPS";

static void gps_task(void *pvParameters) {
    while (1) {
        // Dummy GPS parser
        ESP_LOGD(TAG, "Parsing GPS...");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void gps_parser_init(void) {
    xTaskCreate(gps_task, "gps_task", 2048, NULL, 5, NULL);
}
