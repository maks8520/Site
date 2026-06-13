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
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "mpu6050.h"

#define TAG "UDOCHKA_C3"

#define HALL_SENSOR_PIN GPIO_NUM_4
#define MPU6050_INT_PIN GPIO_NUM_5

#define LED_RED_PIN   GPIO_NUM_18
#define LED_GREEN_PIN GPIO_NUM_19
#define LED_BLUE_PIN  GPIO_NUM_20

#define BATTERY_ADC_CHANNEL ADC_CHANNEL_0 // GPIO0

#define NVS_NAMESPACE "storage"
#define NVS_CHANNEL_KEY "wifi_channel"
#define NVS_CAL_X_KEY "cal_x"
#define NVS_CAL_Z_KEY "cal_z"

static uint8_t dest_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static TaskHandle_t main_task_handle = NULL;
static volatile bool esp_now_ack_received = false;
static volatile bool calibration_requested = false;

static adc_oneshot_unit_handle_t adc1_handle;
static adc_cali_handle_t adc1_cali_handle = NULL;
static bool do_calibration_adc1 = false;

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
    uint8_t channel = 1;

    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &my_handle) == ESP_OK) {
        nvs_get_u8(my_handle, NVS_CHANNEL_KEY, &channel);
        nvs_close(my_handle);
    }
    return channel;
}

static void save_wifi_channel(uint8_t channel) {
    nvs_handle_t my_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &my_handle) == ESP_OK) {
        if (nvs_set_u8(my_handle, NVS_CHANNEL_KEY, channel) == ESP_OK) {
            nvs_commit(my_handle);
        }
        nvs_close(my_handle);
    }
}

static void load_calibration(int16_t *cal_x, int16_t *cal_z) {
    nvs_handle_t my_handle;
    *cal_x = 0;
    *cal_z = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &my_handle) == ESP_OK) {
        nvs_get_i16(my_handle, NVS_CAL_X_KEY, cal_x);
        nvs_get_i16(my_handle, NVS_CAL_Z_KEY, cal_z);
        nvs_close(my_handle);
    }
}

static void save_calibration(int16_t cal_x, int16_t cal_z) {
    nvs_handle_t my_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &my_handle) == ESP_OK) {
        nvs_set_i16(my_handle, NVS_CAL_X_KEY, cal_x);
        nvs_set_i16(my_handle, NVS_CAL_Z_KEY, cal_z);
        nvs_commit(my_handle);
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
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
}

static void espnow_send_cb(const uint8_t *mac_addr, esp_now_send_status_t status) {
    esp_now_ack_received = (status == ESP_NOW_SEND_SUCCESS);
    if (main_task_handle != NULL) {
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        vTaskNotifyGiveFromISR(main_task_handle, &xHigherPriorityTaskWoken);
        if (xHigherPriorityTaskWoken) {
            portYIELD_FROM_ISR();
        }
    }
}

static void espnow_recv_cb(const esp_now_recv_info_t *esp_now_info, const uint8_t *data, int data_len) {
    char *cmd_str = strndup((const char *)data, data_len);
    if (cmd_str) {
        ESP_LOGI(TAG, "Recv: %s", cmd_str);
        if (strstr(cmd_str, "calibrate")) {
            calibration_requested = true;
        }
        free(cmd_str);
    }
}

static void espnow_init() {
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_send_cb(espnow_send_cb));
    ESP_ERROR_CHECK(esp_now_register_recv_cb(espnow_recv_cb));

    esp_now_peer_info_t peer_info = {};
    memcpy(peer_info.peer_addr, dest_mac, 6);
    peer_info.channel = 0;
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

static void adc_init() {
    adc_oneshot_unit_init_cfg_t init_config1 = {
        .unit_id = ADC_UNIT_1,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config1, &adc1_handle));

    adc_oneshot_chan_cfg_t config = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = ADC_ATTEN_DB_11,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, BATTERY_ADC_CHANNEL, &config));

    adc_cali_curve_fitting_config_t cali_config = {
        .unit_id = ADC_UNIT_1,
        .atten = ADC_ATTEN_DB_11,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_config, &adc1_cali_handle) == ESP_OK) {
        do_calibration_adc1 = true;
    }
}

static int get_battery_voltage() {
    int adc_raw;
    int voltage = 0;
    ESP_ERROR_CHECK(adc_oneshot_read(adc1_handle, BATTERY_ADC_CHANNEL, &adc_raw));
    if (do_calibration_adc1) {
        ESP_ERROR_CHECK(adc_cali_raw_to_voltage(adc1_cali_handle, adc_raw, &voltage));
    } else {
        voltage = adc_raw; // fallback
    }
    return voltage * 2; // Example multiplier
}

static bool send_telemetry_hunter(int16_t acc_x, int16_t acc_z, int hall_val, int battery_mv) {
    char payload[256];
    snprintf(payload, sizeof(payload), "{\"telemetry\": {\"acc_x\": %d, \"acc_z\": %d, \"hall\": %d}, \"battery_mV\": %d}", acc_x, acc_z, hall_val, battery_mv);

    uint8_t current_channel = load_wifi_channel();
    ESP_LOGI(TAG, "Sending on channel: %d", current_channel);

    led_set(0, 0, 1);
    main_task_handle = xTaskGetCurrentTaskHandle();

    for (int i = 0; i < 13; i++) {
        uint8_t ch = (current_channel - 1 + i) % 13 + 1;
        esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);

        esp_now_ack_received = false;
        ulTaskNotifyTake(pdTRUE, 0);

        esp_now_send(dest_mac, (const uint8_t *)payload, strlen(payload));

        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5));

        if (esp_now_ack_received) {
            ESP_LOGI(TAG, "ACK received on channel %d", ch);
            if (ch != current_channel) save_wifi_channel(ch);

            led_set(0, 1, 0);
            vTaskDelay(pdMS_TO_TICKS(500));
            led_set(0, 0, 0);
            main_task_handle = NULL;
            return true;
        }
    }

    led_set(1, 0, 0);
    vTaskDelay(pdMS_TO_TICKS(500));
    led_set(0, 0, 0);
    main_task_handle = NULL;
    return false;
}

void app_main(void) {
    esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();

    init_nvs();
    leds_init();
    adc_init();

    gpio_config_t hall_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << HALL_SENSOR_PIN),
        .pull_down_en = 0,
        .pull_up_en = 1
    };
    gpio_config(&hall_conf);

    // Initialize I2C unconditionally for both normal ops and calibration requests
    if (i2c_master_init() == ESP_OK) {
        mpu6050_init();
    } else {
        ESP_LOGE(TAG, "Failed to initialize I2C");
    }

    wifi_init();
    espnow_init();

    int battery_mv = get_battery_voltage();

    if (wakeup_reason == ESP_SLEEP_WAKEUP_TIMER) {
        // Battery ping
        ESP_LOGI(TAG, "Timer Wakeup - Battery Ping");
        send_telemetry_hunter(0, 0, 0, battery_mv);
    } else {
        // Sensor or external wakeup
        ESP_LOGI(TAG, "Sensor Wakeup");
        int16_t raw_x = 0, raw_z = 0;
        mpu6050_read_accel(&raw_x, &raw_z);

        int16_t cal_x = 0, cal_z = 0;
        load_calibration(&cal_x, &cal_z);

        int16_t acc_x = raw_x - cal_x;
        int16_t acc_z = raw_z - cal_z;

        int hall_val = gpio_get_level(HALL_SENSOR_PIN);

        send_telemetry_hunter(acc_x, acc_z, hall_val, battery_mv);
    }

    // Process calibration command if received during the 500ms success delay in send_telemetry_hunter
    if (calibration_requested) {
        int16_t raw_x = 0, raw_z = 0;
        mpu6050_read_accel(&raw_x, &raw_z);
        save_calibration(raw_x, raw_z);
        ESP_LOGI(TAG, "Calibrated offsets: X=%d, Z=%d", raw_x, raw_z);
    }

    // Configure wakeups
    esp_deep_sleep_enable_gpio_wakeup((1ULL << MPU6050_INT_PIN), ESP_GPIO_WAKEUP_GPIO_HIGH);
    esp_deep_sleep_enable_gpio_wakeup((1ULL << HALL_SENSOR_PIN), ESP_GPIO_WAKEUP_GPIO_LOW);

    // Wake up every 30 minutes (30 * 60 * 1000000 us)
    esp_sleep_enable_timer_wakeup(1800000000ULL);

    ESP_LOGI(TAG, "Entering deep sleep");
    esp_deep_sleep_start();
}
