#include "network_watchdog.h"

#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "esp_log.h"
#include "wifi_manager.h"

static const char *TAG = "WATCHDOG";

static TimerHandle_t s_watchdog_timer;

static void watchdog_timeout_cb(TimerHandle_t timer)
{
    (void)timer;
    ESP_LOGW(TAG, "No network activity for 5 minutes, restarting Wi-Fi");
    wifi_manager_restart();
}

void network_watchdog_feed(void)
{
    if (s_watchdog_timer != NULL) {
        xTimerReset(s_watchdog_timer, 0);
    }
}

void network_watchdog_init(void)
{
    s_watchdog_timer = xTimerCreate("net_wdg",
                                     pdMS_TO_TICKS(5 * 60 * 1000),
                                     pdFALSE,
                                     NULL,
                                     watchdog_timeout_cb);
    if (s_watchdog_timer == NULL) {
        ESP_LOGE(TAG, "Failed to create watchdog timer");
        return;
    }

    xTimerStart(s_watchdog_timer, 0);
}
