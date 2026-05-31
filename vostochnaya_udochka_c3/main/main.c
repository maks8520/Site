#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "driver/adc.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_sleep.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "mpu6050.h"

#define TAG "UDOCHKA_C3"

#define DATABASE_MAC_0 0x28
#define DATABASE_MAC_1 0x84
#define DATABASE_MAC_2 0x85
#define DATABASE_MAC_3 0x50
#define DATABASE_MAC_4 0x79
#define DATABASE_MAC_5 0x5D

#define HALL_SENSOR_PIN GPIO_NUM_4
#define MPU6050_INT_PIN GPIO_NUM_5

#define LED_RED_PIN GPIO_NUM_18
#define LED_GREEN_PIN GPIO_NUM_19
#define LED_BLUE_PIN GPIO_NUM_20

#define BATTERY_ADC_CHANNEL ADC_CHANNEL_0
#define BATTERY_ADC_ATTEN ADC_ATTEN_DB_11

#define NVS_NAMESPACE "storage"
#define NVS_CHANNEL_KEY "wifi_channel"
#define NVS_CAL_X_KEY "cal_x"
#define NVS_CAL_Z_KEY "cal_z"

#define CHANNEL_MIN 1
#define CHANNEL_MAX 13
#define ACK_TIMEOUT_MS 100
#define SLEEP_30_MINUTES_US 1800000000ULL

typedef struct __attribute__((packed)) {
    char type[3];
    uint8_t status;
} app_ack_payload_t;

static const uint8_t s_database_mac[ESP_NOW_ETH_ALEN] = {
    DATABASE_MAC_0,
    DATABASE_MAC_1,
    DATABASE_MAC_2,
    DATABASE_MAC_3,
    DATABASE_MAC_4,
    DATABASE_MAC_5,
};

static SemaphoreHandle_t s_ack_semaphore;
static volatile bool s_app_ack_received;
static uint8_t s_current_seq;
static adc_oneshot_unit_handle_t s_adc_handle;
static adc_cali_handle_t s_adc_cali_handle;
static bool s_adc_calibrated;

static void init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

static void set_rgb(bool red, bool green, bool blue)
{
    gpio_set_level(LED_RED_PIN, red ? 1 : 0);
    gpio_set_level(LED_GREEN_PIN, green ? 1 : 0);
    gpio_set_level(LED_BLUE_PIN, blue ? 1 : 0);
}

static void rgb_init(void)
{
    gpio_config_t config = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << LED_RED_PIN) | (1ULL << LED_GREEN_PIN) | (1ULL << LED_BLUE_PIN),
        .pull_down_en = 0,
        .pull_up_en = 0,
    };
    ESP_ERROR_CHECK(gpio_config(&config));
    set_rgb(false, false, false);
}

static void battery_adc_init(void)
{
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = ADC_UNIT_1,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &s_adc_handle));

    adc_oneshot_chan_cfg_t chan_cfg = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = BATTERY_ADC_ATTEN,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc_handle, BATTERY_ADC_CHANNEL, &chan_cfg));

    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = ADC_UNIT_1,
        .atten = BATTERY_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    s_adc_calibrated = (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_adc_cali_handle) == ESP_OK);
}

static int read_battery_mv(void)
{
    int raw = 0;
    int mv = 0;
    ESP_ERROR_CHECK(adc_oneshot_read(s_adc_handle, BATTERY_ADC_CHANNEL, &raw));
    if (s_adc_calibrated) {
        ESP_ERROR_CHECK(adc_cali_raw_to_voltage(s_adc_cali_handle, raw, &mv));
    } else {
        mv = raw;
    }
    return mv * 2;
}

static void hall_sensor_init(void)
{
    gpio_config_t config = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = 1ULL << HALL_SENSOR_PIN,
        .pull_down_en = 0,
        .pull_up_en = 1,
    };
    ESP_ERROR_CHECK(gpio_config(&config));
}

static void wifi_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
}

static uint8_t load_wifi_channel(void)
{
    uint8_t channel = 1;
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        if (nvs_get_u8(handle, NVS_CHANNEL_KEY, &channel) != ESP_OK) {
            channel = 1;
        }
        nvs_close(handle);
    }
    if (channel < CHANNEL_MIN || channel > CHANNEL_MAX) {
        channel = 1;
    }
    return channel;
}

static void save_wifi_channel(uint8_t channel)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_u8(handle, NVS_CHANNEL_KEY, channel);
        nvs_commit(handle);
        nvs_close(handle);
    }
}

static void load_calibration(int16_t *cal_x, int16_t *cal_z)
{
    nvs_handle_t handle;
    *cal_x = 0;
    *cal_z = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        nvs_get_i16(handle, NVS_CAL_X_KEY, cal_x);
        nvs_get_i16(handle, NVS_CAL_Z_KEY, cal_z);
        nvs_close(handle);
    }
}

static void save_calibration(int16_t cal_x, int16_t cal_z)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_i16(handle, NVS_CAL_X_KEY, cal_x);
        nvs_set_i16(handle, NVS_CAL_Z_KEY, cal_z);
        nvs_commit(handle);
        nvs_close(handle);
    }
}

static void ensure_peer(void)
{
    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, s_database_mac, ESP_NOW_ETH_ALEN);
    peer.ifidx = WIFI_IF_STA;
    peer.channel = 0;
    peer.encrypt = false;
    if (!esp_now_is_peer_exist(s_database_mac)) {
        ESP_ERROR_CHECK(esp_now_add_peer(&peer));
    }
}

static void app_ack_reset(void)
{
    s_app_ack_received = false;
    xSemaphoreTake(s_ack_semaphore, 0);
}

static bool wait_for_app_ack(void)
{
    if (xSemaphoreTake(s_ack_semaphore, pdMS_TO_TICKS(ACK_TIMEOUT_MS)) == pdTRUE) {
        return s_app_ack_received;
    }
    return false;
}

static void espnow_send_cb(const uint8_t *mac_addr, esp_now_send_status_t status)
{
    (void)mac_addr;
    (void)status;
}

static void espnow_recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int data_len)
{
    if (info == NULL || data == NULL || data_len <= 0) {
        return;
    }

    if (memcmp(info->src_addr, s_database_mac, ESP_NOW_ETH_ALEN) != 0) {
        return;
    }

    char *payload = malloc((size_t)data_len + 1);
    if (payload == NULL) {
        return;
    }
    memcpy(payload, data, (size_t)data_len);
    payload[data_len] = '\0';

    if (strstr(payload, "\"type\":\"ack\"") != NULL && strstr(payload, "\"status\":\"ok\"") != NULL) {
        s_app_ack_received = true;
        xSemaphoreGive(s_ack_semaphore);
    }

    free(payload);
}

static void espnow_init(void)
{
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_send_cb(espnow_send_cb));
    ESP_ERROR_CHECK(esp_now_register_recv_cb(espnow_recv_cb));
    ensure_peer();
}

static void flash_blue_scan(void)
{
    set_rgb(false, false, true);
    vTaskDelay(pdMS_TO_TICKS(120));
    set_rgb(false, false, false);
}

static void flash_green_success(void)
{
    set_rgb(false, true, false);
    vTaskDelay(pdMS_TO_TICKS(500));
    set_rgb(false, false, false);
}

static void flash_red_error(void)
{
    set_rgb(true, false, false);
    vTaskDelay(pdMS_TO_TICKS(500));
    set_rgb(false, false, false);
}

static bool send_once_on_channel(uint8_t channel, const char *payload)
{
    ESP_ERROR_CHECK(esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE));
    ensure_peer();
    app_ack_reset();

    if (esp_now_send(s_database_mac, (const uint8_t *)payload, strlen(payload)) != ESP_OK) {
        return false;
    }

    return wait_for_app_ack();
}

static bool send_telemetry_hunter(const char *payload)
{
    uint8_t preferred_channel = load_wifi_channel();

    if (send_once_on_channel(preferred_channel, payload)) {
        return true;
    }

    for (uint8_t channel = CHANNEL_MIN; channel <= CHANNEL_MAX; ++channel) {
        if (channel == preferred_channel) {
            continue;
        }

        flash_blue_scan();
        if (send_once_on_channel(channel, payload)) {
            save_wifi_channel(channel);
            return true;
        }
    }

    return false;
}

static void configure_sleep_wakeups(void)
{
    ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup(SLEEP_30_MINUTES_US));
    ESP_ERROR_CHECK(esp_deep_sleep_enable_gpio_wakeup((1ULL << MPU6050_INT_PIN), ESP_GPIO_WAKEUP_GPIO_HIGH));
    ESP_ERROR_CHECK(esp_deep_sleep_enable_gpio_wakeup((1ULL << HALL_SENSOR_PIN), ESP_GPIO_WAKEUP_GPIO_LOW));
}

static void build_telemetry_payload(char *buffer, size_t buffer_len, int16_t acc_x, int16_t acc_z, int hall_value)
{
    snprintf(buffer, buffer_len, "{\"telemetry\":{\"acc_x\":%d,\"acc_z\":%d,\"hall\":%d}}", acc_x, acc_z, hall_value);
}

static void take_calibration_sample(void)
{
    int16_t raw_x = 0;
    int16_t raw_z = 0;
    if (mpu6050_read_accel(&raw_x, &raw_z) == ESP_OK) {
        save_calibration(raw_x, raw_z);
        ESP_LOGI(TAG, "Calibration stored: x=%d z=%d", raw_x, raw_z);
    }
}

void app_main(void)
{
    init_nvs();
    rgb_init();
    battery_adc_init();
    hall_sensor_init();
    ESP_ERROR_CHECK(mpu6050_init());
    wifi_init();

    s_ack_semaphore = xSemaphoreCreateBinary();
    if (s_ack_semaphore == NULL) {
        ESP_LOGE(TAG, "Failed to create ACK semaphore");
        flash_red_error();
        esp_deep_sleep_start();
    }

    espnow_init();

    int battery_mv = read_battery_mv();
    ESP_LOGI(TAG, "Battery voltage: %d mV", battery_mv);

    int16_t cal_x = 0;
    int16_t cal_z = 0;
    load_calibration(&cal_x, &cal_z);

    int16_t acc_x = 0;
    int16_t acc_z = 0;
    if (mpu6050_read_accel(&acc_x, &acc_z) != ESP_OK) {
        acc_x = 0;
        acc_z = 0;
    } else {
        acc_x -= cal_x;
        acc_z -= cal_z;
    }

    const int hall_value = gpio_get_level(HALL_SENSOR_PIN);
    char telemetry[128];
    build_telemetry_payload(telemetry, sizeof(telemetry), acc_x, acc_z, hall_value);

    bool success = send_telemetry_hunter(telemetry);
    if (success) {
        flash_green_success();
    } else {
        flash_red_error();
    }

    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER) {
        ESP_LOGI(TAG, "Timer wake used for battery measurement");
    }

    take_calibration_sample();
    configure_sleep_wakeups();
    ESP_LOGI(TAG, "Entering deep sleep");
    esp_deep_sleep_start();
}
