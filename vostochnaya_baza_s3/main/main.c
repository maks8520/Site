#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_http_server.h"
#include "lwip/sockets.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "gps_parser.h"
#include "esp_now_receiver.h"

#define BUTTON_1_GPIO 4
#define BUTTON_2_GPIO 5
#define PIN_NUM_MISO 13
#define PIN_NUM_MOSI 11
#define PIN_NUM_CLK  12
#define PIN_NUM_CS   10

static const char *TAG = "MAIN";

// FreeRTOS Queues
QueueHandle_t log_queue;
static httpd_handle_t server = NULL;
// Store a list of active websocket descriptors for broadcasting
#define MAX_CLIENTS 10
static int ws_clients[MAX_CLIENTS] = {0};

static void wifi_init_sta(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = "YOUR_SSID",
            .password = "YOUR_PASS",
            // Need to set proper threshold and auth mode in real use
        },
    };

    // Explicitly set STA mode
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_connect());

    ESP_LOGI(TAG, "wifi_init_sta finished.");
}

typedef struct {
    char log_message[256];
} log_event_t;

static void log_task(void *pvParameters) {
    log_event_t evt;
    while(1) {
        if (xQueueReceive(log_queue, &evt, portMAX_DELAY) == pdTRUE) {
            FILE* f = fopen("/sdcard/log.txt", "a");
            if (f != NULL) {
                fprintf(f, "%s\n", evt.log_message);
                fclose(f);
            } else {
                ESP_LOGE(TAG, "Failed to open log.txt for writing");
            }
        }
    }
}

static void sd_card_init(void) {
    esp_err_t ret;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = true,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };

    sdmmc_card_t *card;
    const char mount_point[] = "/sdcard";
    ESP_LOGI(TAG, "Initializing SD card");

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = PIN_NUM_MISO,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };

    ret = spi_bus_initialize(host.slot, &bus_cfg, SDSPI_DEFAULT_HOST);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize bus.");
        return;
    }

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = PIN_NUM_CS;
    slot_config.host_id = host.slot;

    ret = esp_vfs_fat_sdspi_mount(mount_point, &host, &slot_config, &mount_config, &card);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount filesystem.");
        return;
    }
    ESP_LOGI(TAG, "Filesystem mounted");

    log_queue = xQueueCreate(10, sizeof(log_event_t));
    xTaskCreate(log_task, "log_task", 4096, NULL, 5, NULL);
}

static void add_client(int fd) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (ws_clients[i] == 0) {
            ws_clients[i] = fd;
            return;
        }
    }
}

static void remove_client(int fd) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (ws_clients[i] == fd) {
            ws_clients[i] = 0;
            return;
        }
    }
}

// Keep track of client liveness (basic)
static void ws_cleanup(void) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (ws_clients[i] != 0) {
            int error = 0;
            socklen_t len = sizeof (error);
            int retval = getsockopt(ws_clients[i], SOL_SOCKET, SO_ERROR, &error, &len);
            if (retval != 0 || error != 0) {
                ws_clients[i] = 0;
            }
        }
    }
}

static esp_err_t ws_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "Handshake done, the new connection was opened");
        add_client(httpd_req_to_sockfd(req));
        return ESP_OK;
    }

    httpd_ws_frame_t ws_pkt;
    uint8_t *buf = NULL;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;

    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_ws_recv_frame failed to get frame len with %d", ret);
        remove_client(httpd_req_to_sockfd(req));
        return ret;
    }

    if (ws_pkt.len) {
        buf = calloc(1, ws_pkt.len + 1);
        if (buf == NULL) {
            ESP_LOGE(TAG, "Failed to calloc memory for buf");
            return ESP_ERR_NO_MEM;
        }
        ws_pkt.payload = buf;
        ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "httpd_ws_recv_frame failed with %d", ret);
            free(buf);
            remove_client(httpd_req_to_sockfd(req));
            return ret;
        }
        ESP_LOGI(TAG, "Got packet with message: %s", ws_pkt.payload);
        free(buf);
    }
    return ret;
}

static void broadcast_ws_message(const char* message) {
    if (!server) return;

    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.payload = (uint8_t*)message;
    ws_pkt.len = strlen(message);
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;

    ws_cleanup(); // Clean up dead sockets before sending

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (ws_clients[i] != 0) {
            esp_err_t ret = httpd_ws_send_frame_async(server, ws_clients[i], &ws_pkt);
            if (ret != ESP_OK) {
                ws_clients[i] = 0; // Remove on send error
            }
        }
    }
}

static httpd_handle_t start_webserver(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.core_id = 1;
    config.stack_size = 10240;
    config.server_port = 80;

    httpd_handle_t server = NULL;
    ESP_LOGI(TAG, "Starting server on port: '%d'", config.server_port);

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t ws = {
            .uri        = "/ws",
            .method     = HTTP_GET,
            .handler    = ws_handler,
            .user_ctx   = NULL,
            .is_websocket = true
        };
        httpd_register_uri_handler(server, &ws);
        return server;
    }

    ESP_LOGI(TAG, "Error starting server!");
    return NULL;
}

static void button_task(void *pvParameters) {
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL<<BUTTON_1_GPIO) | (1ULL<<BUTTON_2_GPIO),
        .pull_down_en = 0,
        .pull_up_en = 1
    };
    gpio_config(&io_conf);

    int btn1_state = 1, btn2_state = 1;
    int btn1_last = 1, btn2_last = 1;

    while (1) {
        int r1 = gpio_get_level(BUTTON_1_GPIO);
        int r2 = gpio_get_level(BUTTON_2_GPIO);
        bool change = false;

        if (r1 != btn1_last) {
            vTaskDelay(50 / portTICK_PERIOD_MS);
            r1 = gpio_get_level(BUTTON_1_GPIO);
            if (r1 != btn1_state) {
                btn1_state = r1;
                ESP_LOGI(TAG, "Button 1 state: %d", btn1_state);
                change = true;
            }
        }

        if (r2 != btn2_last) {
            vTaskDelay(50 / portTICK_PERIOD_MS);
            r2 = gpio_get_level(BUTTON_2_GPIO);
            if (r2 != btn2_state) {
                btn2_state = r2;
                ESP_LOGI(TAG, "Button 2 state: %d", btn2_state);
                change = true;
            }
        }

        btn1_last = r1;
        btn2_last = r2;
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

static void esp_now_forwarder_task(void *pvParameters) {
    esp_now_event_t evt;
    char json_buf[300];

    while(1) {
        if (xQueueReceive(esp_now_queue, &evt, portMAX_DELAY) == pdTRUE) {
            snprintf(json_buf, sizeof(json_buf),
                     "{\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"data\":\"%s\"}",
                     evt.mac_addr[0], evt.mac_addr[1], evt.mac_addr[2],
                     evt.mac_addr[3], evt.mac_addr[4], evt.mac_addr[5],
                     evt.payload);

            broadcast_ws_message(json_buf);

            // Also log to SD
            log_event_t log_evt;
            snprintf(log_evt.log_message, sizeof(log_evt.log_message), "%s", json_buf);
            xQueueSend(log_queue, &log_evt, 0);
        }
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "Vostochnaya Baza S3 starting...");

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    wifi_init_sta();
    sd_card_init();

    // GPS
    gps_init();
    xTaskCreatePinnedToCore(gps_task, "gps_task", 4096, NULL, 5, NULL, 0);

    // Web Server
    server = start_webserver();

    // Buttons
    xTaskCreatePinnedToCore(button_task, "button_task", 2048, NULL, 5, NULL, 1);

    // ESP-NOW
    esp_now_receiver_init();
    xTaskCreatePinnedToCore(esp_now_forwarder_task, "now_fwd_task", 4096, NULL, 5, NULL, 1);
}
