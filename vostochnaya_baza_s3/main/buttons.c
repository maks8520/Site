#include "buttons.h"

#include <stdbool.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define BUTTON_1_GPIO GPIO_NUM_4
#define BUTTON_2_GPIO GPIO_NUM_5

static const char *TAG = "BUTTONS";

static button_callback_t s_button1_callback;
static button_callback_t s_button2_callback;

static void buttons_task(void *arg)
{
    (void)arg;

    const TickType_t poll_delay = pdMS_TO_TICKS(10);
    const int debounce_limit = 5;

    int button1_stable = 0;
    int button2_stable = 0;
    bool button1_active = false;
    bool button2_active = false;

    while (1) {
        bool button1_pressed = gpio_get_level(BUTTON_1_GPIO) == 0;
        bool button2_pressed = gpio_get_level(BUTTON_2_GPIO) == 0;

        if (button1_pressed) {
            if (++button1_stable >= debounce_limit && !button1_active) {
                button1_active = true;
                if (s_button1_callback != NULL) {
                    s_button1_callback();
                }
            }
        } else {
            button1_stable = 0;
            button1_active = false;
        }

        if (button2_pressed) {
            if (++button2_stable >= debounce_limit && !button2_active) {
                button2_active = true;
                if (s_button2_callback != NULL) {
                    s_button2_callback();
                }
            }
        } else {
            button2_stable = 0;
            button2_active = false;
        }

        vTaskDelay(poll_delay);
    }
}

void buttons_init(button_callback_t button1_callback, button_callback_t button2_callback)
{
    s_button1_callback = button1_callback;
    s_button2_callback = button2_callback;

    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << BUTTON_1_GPIO) | (1ULL << BUTTON_2_GPIO),
        .pull_up_en = 1,
        .pull_down_en = 0,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    ESP_LOGI(TAG, "Buttons initialized on GPIO %d and %d", BUTTON_1_GPIO, BUTTON_2_GPIO);
    xTaskCreate(buttons_task, "buttons", 2048, NULL, 3, NULL);
}
