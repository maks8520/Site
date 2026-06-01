#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"

static const char *TAG = "UDOCHKA";

static void wifi_init(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
}

static void calibrate_echosounder(void) {
    ESP_LOGI(TAG, "Calibrating echosounder...");
    vTaskDelay(pdMS_TO_TICKS(1000));
    ESP_LOGI(TAG, "Calibration complete.");
}

static void ping_battery(void) {
    int battery_level = 85; // Mock battery percentage
    ESP_LOGI(TAG, "Battery level: %d%%", battery_level);
}

void app_main(void) {
    printf("Vostochnaya Udochka C3 started\n");

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    wifi_init();
    calibrate_echosounder();

    while(1) {
        ping_battery();
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
