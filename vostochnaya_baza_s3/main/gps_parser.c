#include "gps_parser.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "web_server.h"

#define GPS_UART UART_NUM_1
#define GPS_UART_TX GPIO_NUM_1
#define GPS_UART_RX GPIO_NUM_2
#define GPS_UART_BAUD 9600

static const char *TAG = "GPS";

static portMUX_TYPE s_fix_mux = portMUX_INITIALIZER_UNLOCKED;
static double s_latitude;
static double s_longitude;
static bool s_has_fix;
static char s_utc_time[16];

static double nmea_to_decimal(const char *value, char direction)
{
    if (value == NULL || value[0] == '\0') {
        return 0.0;
    }

    double raw = strtod(value, NULL);
    double degrees = floor(raw / 100.0);
    double minutes = raw - (degrees * 100.0);
    double decimal = degrees + (minutes / 60.0);

    if (direction == 'S' || direction == 'W') {
        decimal = -decimal;
    }

    return decimal;
}

static void gps_sync_time_from_utc(const char *utc)
{
    if (utc == NULL || strlen(utc) < 6) {
        return;
    }

    int hour = 0;
    int minute = 0;
    int second = 0;
    if (sscanf(utc, "%2d%2d%2d", &hour, &minute, &second) != 3) {
        return;
    }

    time_t now = time(NULL);
    struct tm current_tm;
    gmtime_r(&now, &current_tm);
    current_tm.tm_hour = hour;
    current_tm.tm_min = minute;
    current_tm.tm_sec = second;
    setenv("TZ", "UTC0", 1);
    tzset();

    time_t synced = mktime(&current_tm);
    struct timeval tv = {
        .tv_sec = synced,
        .tv_usec = 0,
    };
    settimeofday(&tv, NULL);
    ESP_LOGI(TAG, "GPS time synchronized to UTC %02d:%02d:%02d", hour, minute, second);
}

static void gps_publish_fix(double latitude, double longitude, const char *utc)
{
    char message[192];
    snprintf(message, sizeof(message), "{\"gps\":{\"lat\":%.6f,\"lon\":%.6f,\"utc\":\"%s\"}}",
             latitude,
             longitude,
             utc != NULL ? utc : "");
    web_server_send_text_async(message);
}

static void parse_gga_sentence(char *sentence)
{
    char *saveptr = NULL;
    char *field = strtok_r(sentence, ",", &saveptr);
    int index = 0;

    char utc[16] = {0};
    char latitude_field[24] = {0};
    char latitude_direction = 'N';
    char longitude_field[24] = {0};
    char longitude_direction = 'E';
    char fix_quality = '0';

    while (field != NULL) {
        if (index == 1) {
            strncpy(utc, field, sizeof(utc) - 1);
        } else if (index == 2) {
            strncpy(latitude_field, field, sizeof(latitude_field) - 1);
        } else if (index == 3 && field[0] != '\0') {
            latitude_direction = field[0];
        } else if (index == 4) {
            strncpy(longitude_field, field, sizeof(longitude_field) - 1);
        } else if (index == 5 && field[0] != '\0') {
            longitude_direction = field[0];
        } else if (index == 6 && field[0] != '\0') {
            fix_quality = field[0];
        }

        field = strtok_r(NULL, ",", &saveptr);
        index++;
    }

    if (fix_quality == '0' || latitude_field[0] == '\0' || longitude_field[0] == '\0') {
        return;
    }

    double latitude = nmea_to_decimal(latitude_field, latitude_direction);
    double longitude = nmea_to_decimal(longitude_field, longitude_direction);

    portENTER_CRITICAL(&s_fix_mux);
    s_latitude = latitude;
    s_longitude = longitude;
    s_has_fix = true;
    strncpy(s_utc_time, utc, sizeof(s_utc_time) - 1);
    s_utc_time[sizeof(s_utc_time) - 1] = '\0';
    portEXIT_CRITICAL(&s_fix_mux);

    gps_sync_time_from_utc(utc);
    gps_publish_fix(latitude, longitude, utc);

    ESP_LOGI(TAG, "GPS fix lat=%.6f lon=%.6f", latitude, longitude);
}

static void gps_uart_task(void *arg)
{
    (void)arg;

    uint8_t byte;
    char line[160];
    size_t line_len = 0;

    while (1) {
        int received = uart_read_bytes(GPS_UART, &byte, 1, pdMS_TO_TICKS(100));
        if (received <= 0) {
            continue;
        }

        if (byte == '\r') {
            continue;
        }

        if (byte == '\n') {
            line[line_len] = '\0';
            if (strncmp(line, "$GPGGA", 6) == 0 || strncmp(line, "$GNGGA", 6) == 0) {
                char sentence[160];
                strncpy(sentence, line, sizeof(sentence) - 1);
                sentence[sizeof(sentence) - 1] = '\0';
                parse_gga_sentence(sentence);
            }
            line_len = 0;
            continue;
        }

        if (line_len < sizeof(line) - 1) {
            line[line_len++] = (char)byte;
        } else {
            line_len = 0;
        }
    }
}

void gps_parser_init(void)
{
    uart_config_t config = {
        .baud_rate = GPS_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(GPS_UART, 2048, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(GPS_UART, &config));
    ESP_ERROR_CHECK(uart_set_pin(GPS_UART, GPS_UART_TX, GPS_UART_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    xTaskCreate(gps_uart_task, "gps_uart", 4096, NULL, 5, NULL);
}

bool gps_parser_get_fix(double *latitude, double *longitude, char *utc_time, size_t utc_time_len)
{
    bool has_fix;

    portENTER_CRITICAL(&s_fix_mux);
    has_fix = s_has_fix;
    if (has_fix) {
        if (latitude != NULL) {
            *latitude = s_latitude;
        }
        if (longitude != NULL) {
            *longitude = s_longitude;
        }
        if (utc_time != NULL && utc_time_len > 0) {
            strncpy(utc_time, s_utc_time, utc_time_len - 1);
            utc_time[utc_time_len - 1] = '\0';
        }
    }
    portEXIT_CRITICAL(&s_fix_mux);

    return has_fix;
}

bool gps_parser_has_fix(void)
{
    portENTER_CRITICAL(&s_fix_mux);
    bool has_fix = s_has_fix;
    portEXIT_CRITICAL(&s_fix_mux);
    return has_fix;
}
