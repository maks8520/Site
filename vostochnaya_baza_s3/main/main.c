#include <stdio.h>

#include "metar_fetcher.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "nvs_flash.h"

#include "buttons.h"
#include "esp_now_receiver.h"
#include "gps_parser.h"
#include "network_watchdog.h"
#include "web_server.h"
#include "wifi_manager.h"

static void button1_handler(void)
{
    metar_fetcher_request_now();
}

static void button2_handler(void)
{
    wifi_manager_restart();
}

static void init_nvs_storage(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
}

void app_main(void)
{
    puts("Vostochnaya Baza S3 booting");

    init_nvs_storage();
    wifi_manager_init();
    web_server_init();
    network_watchdog_init();
    esp_now_receiver_init();
    gps_parser_init();
    metar_fetcher_init();
    buttons_init(button1_handler, button2_handler);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
