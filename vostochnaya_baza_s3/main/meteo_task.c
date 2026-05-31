#include "meteo_task.h"
#include "openmeteo_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"

static const char *TAG = "METEO_TASK";
bool serve_from_cache = false;

static void reconnect_wifi() {
    int retries = 0;
    while (retries < 3) {
        ESP_LOGI(TAG, "Attempting WiFi reconnect %d/3", retries + 1);

        esp_wifi_connect();

        // Wait up to 10 seconds for IP address
        // Note: For a robust implementation, we'd wait for an event group bit set by the IP_EVENT_STA_GOT_IP handler.
        // For this task, we'll check the AP info periodically to see if we are connected.
        for(int i = 0; i < 20; i++) {
            wifi_ap_record_t ap_info;
            if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
                 ESP_LOGI(TAG, "WiFi connected");
                 serve_from_cache = false;
                 return;
            }
            vTaskDelay(pdMS_TO_TICKS(500));
        }

        retries++;
    }
    ESP_LOGE(TAG, "WiFi reconnect failed. Serving from cache.");
    serve_from_cache = true;
}

static void meteo_task_func(void *arg) {
    // Example coords
    double lat = 55.7512;
    double lon = 37.6184;

    while (1) {
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) {
            reconnect_wifi();
        }

        if (!serve_from_cache) {
            bool weather_ok = fetch_and_cache_weather(lat, lon);
            bool wind_ok = fetch_and_cache_wind_profile(lat, lon);

            if (!weather_ok || !wind_ok) {
                ESP_LOGW(TAG, "Failed to fetch data, might need reconnect");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(30 * 60 * 1000)); // 30 minutes
    }
}

void meteo_task_start(void) {
    xTaskCreate(meteo_task_func, "meteo_task", 8192, NULL, 5, NULL);
}
