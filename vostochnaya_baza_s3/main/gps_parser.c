#include "gps_parser.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "string.h"
#include "stdlib.h"

#define TXD_PIN (1)
#define RXD_PIN (2)
#define UART_PORT UART_NUM_1
#define BUF_SIZE (1024)

static const char *TAG = "GPS";

void gps_init(void) {
    const uart_config_t uart_config = {
        .baud_rate = 9600,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    // Install UART driver using an event queue here if needed, but not required
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT, BUF_SIZE * 2, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT, TXD_PIN, RXD_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
}

// Convert NMEA DDMM.MMMM to DD.DDDD
static float convert_to_dd(const char* nmea_coord) {
    if (!nmea_coord || strlen(nmea_coord) < 4) return 0.0;

    char degrees_str[4] = {0};
    char minutes_str[16] = {0};

    // Determine number of digits for degrees based on length/format or assuming 2 for lat, 3 for lon.
    // simpler: dot position minus 2 is length of degrees.
    char *dot_pos = strchr(nmea_coord, '.');
    if (!dot_pos) return 0.0;

    int deg_len = (dot_pos - nmea_coord) - 2;
    if (deg_len <= 0 || deg_len > 3) return 0.0;

    strncpy(degrees_str, nmea_coord, deg_len);
    int min_len = strlen(nmea_coord + deg_len);
    if (min_len >= sizeof(minutes_str)) min_len = sizeof(minutes_str) - 1;
    strncpy(minutes_str, nmea_coord + deg_len, min_len);

    float degrees = atof(degrees_str);
    float minutes = atof(minutes_str);

    return degrees + (minutes / 60.0);
}

void gps_task(void *pvParameters) {
    uint8_t* data = (uint8_t*) malloc(BUF_SIZE);
    char line[128];
    int line_len = 0;

    while (1) {
        // Read data from the UART
        int len = uart_read_bytes(UART_PORT, data, BUF_SIZE - 1, 20 / portTICK_PERIOD_MS);
        if (len > 0) {
            for (int i = 0; i < len; i++) {
                char c = data[i];
                if (c == '\n') {
                    line[line_len] = '\0';
                    if (strncmp(line, "$GPGGA", 6) == 0 || strncmp(line, "$GNGGA", 6) == 0) {
                        // Parse GGA
                        // Example: $GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47
                        int token_idx = 0;
                        float lat = 0.0, lon = 0.0;
                        char lat_dir = 'N', lon_dir = 'E';

                        char *start = line;
                        char *end;
                        char token[32];

                        while(start != NULL) {
                            end = strchr(start, ',');
                            if (end) {
                                int len = end - start;
                                if (len >= sizeof(token)) len = sizeof(token) - 1;
                                strncpy(token, start, len);
                                token[len] = '\0';
                                start = end + 1;
                            } else {
                                strncpy(token, start, sizeof(token)-1);
                                token[sizeof(token)-1] = '\0';
                                start = NULL;
                            }

                            if (token_idx == 2) {
                                lat = convert_to_dd(token);
                            } else if (token_idx == 3) {
                                lat_dir = token[0];
                            } else if (token_idx == 4) {
                                lon = convert_to_dd(token);
                            } else if (token_idx == 5) {
                                lon_dir = token[0];
                            }
                            token_idx++;
                        }

                        if (lat_dir == 'S') lat = -lat;
                        if (lon_dir == 'W') lon = -lon;

                        ESP_LOGI(TAG, "Lat: %f, Lon: %f", lat, lon);
                    }
                    line_len = 0;
                } else if (c != '\r') {
                    if (line_len < sizeof(line) - 1) {
                        line[line_len++] = c;
                    } else {
                        // Overflow, reset
                        line_len = 0;
                    }
                }
            }
        }
    }
    free(data);
    vTaskDelete(NULL);
}
