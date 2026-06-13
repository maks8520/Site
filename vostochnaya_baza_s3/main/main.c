#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"

#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "esp_now.h"
#include "esp_netif.h"
#include "lwip/ip4_addr.h"
#include "esp_http_server.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_now_receiver.h"
#include "esp_spiffs.h"

#define WIFI_SSID "POCO F3"
#define WIFI_PASS "11111111"

#define PIN_BUTTON_1 GPIO_NUM_0
#define PIN_BUTTON_2 GPIO_NUM_4

#define UART_NUM_GPS UART_NUM_1
#define GPS_TX_PIN GPIO_NUM_1
#define GPS_RX_PIN GPIO_NUM_2
#define GPS_BAUD_RATE 9600
#define GPS_BUF_SIZE 1024

static const char *TAG = "BAZA_S3";

static httpd_handle_t server = NULL;
static int client_fd = -1;

static QueueHandle_t log_queue;

// --- Wi-Fi Event Handler ---
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "Wi-Fi disconnected. Reconnecting...");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

// --- HTTP Server and WebSockets ---

struct async_resp_arg {
    httpd_handle_t hd;
    int fd;
    char *payload;
};

static void ws_async_send(void *arg) {
    struct async_resp_arg *resp_arg = (struct async_resp_arg *)arg;
    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.payload = (uint8_t*)resp_arg->payload;
    ws_pkt.len = strlen(resp_arg->payload);
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;

    httpd_ws_send_frame_async(resp_arg->hd, resp_arg->fd, &ws_pkt);
    free(resp_arg->payload);
    free(resp_arg);
}

static void send_ws_payload(const char* payload) {
    if (server == NULL || client_fd < 0) return;

    struct async_resp_arg *resp_arg = malloc(sizeof(struct async_resp_arg));
    if (!resp_arg) return;
    resp_arg->hd = server;
    resp_arg->fd = client_fd;
    resp_arg->payload = strdup(payload);
    if (!resp_arg->payload) {
        free(resp_arg);
        return;
    }

    if (httpd_queue_work(server, ws_async_send, resp_arg) != ESP_OK) {
        free(resp_arg->payload);
        free(resp_arg);
    }
}

static esp_err_t ws_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "WebSocket handshake done");
        client_fd = httpd_req_to_sockfd(req);
        return ESP_OK;
    }
    return ESP_OK;
}

static esp_err_t spiffs_handler(httpd_req_t *req) {
    // Basic handler for SPIFFS static content.
    // In a real app, you would read the requested file and return it.
    char filepath[128];
    snprintf(filepath, sizeof(filepath), "/spiffs%s", req->uri);

    // Default to index.html if root requested
    if (strcmp(req->uri, "/") == 0) {
        snprintf(filepath, sizeof(filepath), "/spiffs/index.html");
    }

    FILE *fd = fopen(filepath, "r");
    if (!fd) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File does not exist");
        return ESP_FAIL;
    }

    if (strstr(filepath, ".html")) {
        httpd_resp_set_type(req, "text/html");
    } else if (strstr(filepath, ".css")) {
        httpd_resp_set_type(req, "text/css");
    } else if (strstr(filepath, ".js")) {
        httpd_resp_set_type(req, "application/javascript");
    }

    char chunk[256];
    size_t chunk_len;
    while ((chunk_len = fread(chunk, 1, sizeof(chunk), fd)) > 0) {
        httpd_resp_send_chunk(req, chunk, chunk_len);
    }
    fclose(fd);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static void start_webserver() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 10240;
    config.core_id = 1;
    config.lru_purge_enable = true;

    httpd_uri_t ws = {
        .uri        = "/ws",
        .method     = HTTP_GET,
        .handler    = ws_handler,
        .user_ctx   = NULL,
        .is_websocket = true
    };

    httpd_uri_t spiffs_file = {
        .uri       = "/*",
        .method    = HTTP_GET,
        .handler   = spiffs_handler,
        .user_ctx  = NULL
    };

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_register_uri_handler(server, &ws);
        httpd_register_uri_handler(server, &spiffs_file);
    }
}

// --- ESP-NOW ---
void on_base_espnow_recv(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len) {
    if (client_fd >= 0) {
        // Assume data is a null-terminated JSON string from Udochka
        // We make sure to null terminate it safely
        char *json_str = malloc(len + 1);
        if (json_str) {
            memcpy(json_str, data, len);
            json_str[len] = '\0';
            send_ws_payload(json_str);
            free(json_str);
        }
    }
}

// --- GPS Task ---
static void gps_task(void *pvParameters) {
    uart_config_t uart_config = {
        .baud_rate = GPS_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_param_config(UART_NUM_GPS, &uart_config);
    uart_set_pin(UART_NUM_GPS, GPS_TX_PIN, GPS_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_driver_install(UART_NUM_GPS, GPS_BUF_SIZE * 2, 0, 0, NULL, 0);

    uint8_t *data = (uint8_t *)malloc(GPS_BUF_SIZE);
    if (!data) {
        vTaskDelete(NULL);
    }

    char line_buffer[128];
    int line_len = 0;

    while (1) {
        int len = uart_read_bytes(UART_NUM_GPS, data, GPS_BUF_SIZE, pdMS_TO_TICKS(100));
        if (len > 0) {
            for (int i = 0; i < len; i++) {
                if (data[i] == '\n') {
                    line_buffer[line_len] = '\0';
                    if (strncmp(line_buffer, "$GPGGA", 6) == 0) {
                        float raw_lat, raw_lon;
                        char lat_dir, lon_dir;
                        int fix_quality, satellites;
                        char time_str[20];
                        float hdop, alt, geoid;

                        // Parse $GPGGA
                        // Example: $GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47
                        int parsed = sscanf(line_buffer, "$GPGGA,%[^,],%f,%c,%f,%c,%d,%d,%f,%f,M,%f,M,,*",
                               time_str, &raw_lat, &lat_dir, &raw_lon, &lon_dir, &fix_quality, &satellites, &hdop, &alt, &geoid);

                        if (parsed >= 7 && fix_quality > 0) {
                            // Convert to decimal degrees
                            float lat_dd = (int)(raw_lat / 100) + fmod(raw_lat, 100.0) / 60.0;
                            if (lat_dir == 'S') lat_dd = -lat_dd;

                            float lon_dd = (int)(raw_lon / 100) + fmod(raw_lon, 100.0) / 60.0;
                            if (lon_dir == 'W') lon_dd = -lon_dd;

                            char json_payload[128];
                            snprintf(json_payload, sizeof(json_payload), "{\"gps\": {\"lat\": %.6f, \"lon\": %.6f, \"satellites\": %d}}",
                                     lat_dd, lon_dd, satellites);
                            send_ws_payload(json_payload);
                        }
                    }
                    line_len = 0;
                } else if (data[i] != '\r' && line_len < sizeof(line_buffer) - 1) {
                    line_buffer[line_len++] = (char)data[i];
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    free(data);
    vTaskDelete(NULL);
}

// --- Buttons Task ---
static void base_buttons_task(void *pvParameters) {
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << PIN_BUTTON_1) | (1ULL << PIN_BUTTON_2),
        .pull_down_en = 0,
        .pull_up_en = 1,
    };
    gpio_config(&io_conf);

    int btn1_last = 1;
    int btn2_last = 1;

    while (1) {
        int btn1 = gpio_get_level(PIN_BUTTON_1);
        int btn2 = gpio_get_level(PIN_BUTTON_2);

        if (btn1 == 0 && btn1_last == 1) {
            vTaskDelay(pdMS_TO_TICKS(50));
            if (gpio_get_level(PIN_BUTTON_1) == 0) {
                ESP_LOGI(TAG, "Button 1 pressed");
            }
        }
        if (btn2 == 0 && btn2_last == 1) {
            vTaskDelay(pdMS_TO_TICKS(50));
            if (gpio_get_level(PIN_BUTTON_2) == 0) {
                ESP_LOGI(TAG, "Button 2 pressed");
            }
        }

        btn1_last = btn1;
        btn2_last = btn2;

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// --- Logger Task (Stub for completeness) ---
static void sd_log_async_task(void *pvParameters) {
    char log_buf[256];
    while (1) {
        if (xQueueReceive(log_queue, log_buf, portMAX_DELAY) == pdTRUE) {
            // Write to SD card logic would go here
            ESP_LOGD(TAG, "SD Log: %s", log_buf);
        }
    }
}

void app_main(void) {
    // 1. Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. Init SPIFFS
    esp_vfs_spiffs_conf_t spiffs_conf = {
      .base_path = "/spiffs",
      .partition_label = NULL,
      .max_files = 5,
      .format_if_mount_failed = true
    };
    esp_vfs_spiffs_register(&spiffs_conf);

    // 3. Initialize Wi-Fi (STA mode)
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // 4. Start HTTP Server with WebSockets
    start_webserver();

    // 5. Initialize ESP-NOW
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_base_espnow_recv));

    // 6. Create queues and tasks
    log_queue = xQueueCreate(10, 256);
    xTaskCreatePinnedToCore(gps_task, "gps_task", 4096, NULL, 3, NULL, 0);
    xTaskCreatePinnedToCore(base_buttons_task, "base_buttons_task", 2048, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(sd_log_async_task, "sd_log_async_task", 4096, NULL, 2, NULL, 1);
}
