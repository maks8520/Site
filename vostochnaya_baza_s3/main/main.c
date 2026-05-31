#include <stdio.h>
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "esp_log.h"

#include "esp_now_receiver.h"
#include "gps_parser.h"
#include "metar_fetcher.h"

static const char *TAG = "MAIN";
TimerHandle_t net_watchdog_timer;

static void net_watchdog_cb(TimerHandle_t xTimer) {
    ESP_LOGW(TAG, "Network watchdog triggered! Restarting Wi-Fi STA...");
    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_wifi_start();
    ESP_LOGI(TAG, "Wi-Fi restarted by Watchdog");
}

void feed_net_watchdog(void) {
    if (net_watchdog_timer) {
        xTimerReset(net_watchdog_timer, 0);
    }
}

static void wifi_init(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
}

void app_main(void) {
    printf("Vostochnaya Baza S3 started\n");

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    wifi_init();
    esp_now_receiver_init();
    gps_parser_init();
    metar_fetcher_init();

    // 5-minute FreeRTOS network watchdog timer
    net_watchdog_timer = xTimerCreate("net_wdg", pdMS_TO_TICKS(5 * 60 * 1000), pdTRUE, (void *)0, net_watchdog_cb);
    xTimerStart(net_watchdog_timer, 0);

    while(1) {
        // Main loop logic
        // E.g., feed_net_watchdog(); when we receive websocket ping/pong
        feed_net_watchdog();
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
