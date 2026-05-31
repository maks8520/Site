#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_now.h"
#include "esp_sleep.h"
#include "driver/gpio.h"
#include "mpu6050.h"

#define TAG "UDOCHKA_C3"

#define HALL_SENSOR_PIN GPIO_NUM_4
#define MPU6050_INT_PIN GPIO_NUM_5

#define LED_RED_PIN   GPIO_NUM_18
#define LED_GREEN_PIN GPIO_NUM_19
#define LED_BLUE_PIN  GPIO_NUM_20

#define NVS_NAMESPACE "storage"
#define NVS_CHANNEL_KEY "wifi_channel"

// Note: ESP-NOW requires a Unicast MAC address for the hardware to trigger an ACK.
// Sending to the Broadcast MAC (FF:FF:FF:FF:FF:FF) will always immediately return SUCCESS
// without actually waiting for a receiver, breaking the channel hopping logic.
// Replace this with the actual base station MAC.
static uint8_t dest_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static TaskHandle_t main_task_handle = NULL;
static volatile bool esp_now_ack_received = false;

static void init_nvs() {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

static uint8_t load_wifi_channel() {
    nvs_handle_t my_handle;
    esp_err_t err;
    uint8_t channel = 1; // Default channel

    err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &my_handle);
    if (err == ESP_OK) {
        err = nvs_get_u8(my_handle, NVS_CHANNEL_KEY, &channel);
        if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGE(TAG, "Error reading Wi-Fi channel from NVS: %s", esp_err_to_name(err));
        }
        nvs_close(my_handle);
    }
    return channel;
}

static void save_wifi_channel(uint8_t channel) {
    nvs_handle_t my_handle;
    esp_err_t err;

    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &my_handle);
    if (err == ESP_OK) {
        err = nvs_set_u8(my_handle, NVS_CHANNEL_KEY, channel);
        if (err == ESP_OK) {
            err = nvs_commit(my_handle);
        }
        nvs_close(my_handle);
    }
}

static void wifi_init() {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    // Disable power saving to ensure fast TX/RX
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
}

static void espnow_send_cb(const uint8_t *mac_addr, esp_now_send_status_t status) {
    if (status == ESP_NOW_SEND_SUCCESS) {
        esp_now_ack_received = true;
    } else {
        esp_now_ack_received = false;
    }
    // Instantly notify main task
    if (main_task_handle != NULL) {
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        vTaskNotifyGiveFromISR(main_task_handle, &xHigherPriorityTaskWoken);
        if (xHigherPriorityTaskWoken) {
            portYIELD_FROM_ISR();
        }
    }
}

static void espnow_init() {
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_send_cb(espnow_send_cb));

    esp_now_peer_info_t peer_info = {};
    memcpy(peer_info.peer_addr, dest_mac, 6);
    peer_info.channel = 0; // use current channel
    peer_info.ifidx = ESP_IF_WIFI_STA;
    peer_info.encrypt = false;
    ESP_ERROR_CHECK(esp_now_add_peer(&peer_info));
}

static void led_set(int r, int g, int b) {
    gpio_set_level(LED_RED_PIN, r);
    gpio_set_level(LED_GREEN_PIN, g);
    gpio_set_level(LED_BLUE_PIN, b);
}

static void leds_init() {
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << LED_RED_PIN) | (1ULL << LED_GREEN_PIN) | (1ULL << LED_BLUE_PIN),
        .pull_down_en = 0,
        .pull_up_en = 0
    };
    gpio_config(&io_conf);
    led_set(0, 0, 0);
}

static bool send_telemetry_hunter(int16_t acc_x, int16_t acc_z, int hall_val) {
    char payload[128];
    snprintf(payload, sizeof(payload), "{\"telemetry\": {\"acc_x\": %d, \"acc_z\": %d, \"hall\": %d}}", acc_x, acc_z, hall_val);

    uint8_t current_channel = load_wifi_channel();
    ESP_LOGI(TAG, "Starting Hunter mode. Initial channel: %d", current_channel);

    // Blue for searching
    led_set(0, 0, 1);

    main_task_handle = xTaskGetCurrentTaskHandle();

    for (int i = 0; i < 13; i++) {
        uint8_t ch = (current_channel - 1 + i) % 13 + 1;
        esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);

        esp_now_ack_received = false;

        // Clear pending notifications
        ulTaskNotifyTake(pdTRUE, 0);

        esp_now_send(dest_mac, (const uint8_t *)payload, strlen(payload));

        // Wait instantly for callback, timeout after 5ms (highly optimized)
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5));

        if (esp_now_ack_received) {
            ESP_LOGI(TAG, "ACK received on channel %d", ch);
            if (ch != current_channel) {
                save_wifi_channel(ch);
            }
            // Green for success
            led_set(0, 1, 0);
            vTaskDelay(pdMS_TO_TICKS(500));
            led_set(0, 0, 0);
            main_task_handle = NULL;
            return true;
        }
    }

    ESP_LOGW(TAG, "Failed to receive ACK on any channel");
    // Red for error
    led_set(1, 0, 0);
    vTaskDelay(pdMS_TO_TICKS(500));
    led_set(0, 0, 0);
    main_task_handle = NULL;
    return false;
}

void app_main(void) {
    ESP_LOGI(TAG, "Vostochnaya Udochka C3 wake up");

    // Initialize systems
    init_nvs();
    leds_init();

    // Config Hall sensor pin as input
    gpio_config_t hall_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << HALL_SENSOR_PIN),
        .pull_down_en = 0,
        .pull_up_en = 1
    };
    gpio_config(&hall_conf);

    // Initialize I2C and MPU6050
    if (i2c_master_init() == ESP_OK) {
        mpu6050_init();
    } else {
        ESP_LOGE(TAG, "Failed to initialize I2C");
    }

    // Read sensors
    int16_t acc_x = 0, acc_z = 0;
    mpu6050_read_accel(&acc_x, &acc_z);
    int hall_val = gpio_get_level(HALL_SENSOR_PIN);

    ESP_LOGI(TAG, "Acc X: %d, Acc Z: %d, Hall: %d", acc_x, acc_z, hall_val);

    // Send data
    wifi_init();
    espnow_init();
    send_telemetry_hunter(acc_x, acc_z, hall_val);

    // Prepare deep sleep
    // Configure MPU6050 INT pin to wake up
    esp_deep_sleep_enable_gpio_wakeup((1ULL << MPU6050_INT_PIN), ESP_GPIO_WAKEUP_GPIO_HIGH);

    // Configure Hall sensor pin to wake up (assuming active low for typical hall effect sensor, adjust if needed)
    // ESP32-C3 supports ext1 wakeup on multiple pins, but esp_deep_sleep_enable_gpio_wakeup is easier for deep sleep
    esp_deep_sleep_enable_gpio_wakeup((1ULL << HALL_SENSOR_PIN), ESP_GPIO_WAKEUP_GPIO_LOW);

    ESP_LOGI(TAG, "Entering deep sleep");
    esp_deep_sleep_start();
}
