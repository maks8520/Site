#include "web_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "WEB";

static httpd_handle_t s_http_server;
static int s_active_ws_fd = -1;

static const char *content_type_from_path(const char *path)
{
    const char *ext = strrchr(path, '.');
    if (ext == NULL) {
        return "text/html";
    }
    if (strcmp(ext, ".html") == 0) {
        return "text/html";
    }
    if (strcmp(ext, ".css") == 0) {
        return "text/css";
    }
    if (strcmp(ext, ".js") == 0) {
        return "application/javascript";
    }
    if (strcmp(ext, ".json") == 0) {
        return "application/json";
    }
    return "application/octet-stream";
}

static esp_err_t static_handler(httpd_req_t *req)
{
    char path[600];
    const char *uri = req->uri;

    if (strcmp(uri, "/") == 0) {
        uri = "/index.html";
    }

    snprintf(path, sizeof(path), "/spiffs%s", uri);

    FILE *file = fopen(path, "r");
    if (file == NULL) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
        return ESP_OK;
    }

    httpd_resp_set_type(req, content_type_from_path(path));

    char buffer[1024];
    size_t read_bytes = 0;
    while ((read_bytes = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        if (httpd_resp_send_chunk(req, buffer, read_bytes) != ESP_OK) {
            fclose(file);
            httpd_resp_sendstr_chunk(req, NULL);
            return ESP_FAIL;
        }
    }

    fclose(file);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static void ws_send_work(void *arg)
{
    char *message = arg;
    if (message == NULL) {
        return;
    }

    if (s_http_server != NULL && s_active_ws_fd >= 0) {
        httpd_ws_frame_t frame = {
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t *)message,
            .len = strlen(message),
        };
        esp_err_t err = httpd_ws_send_frame_async(s_http_server, s_active_ws_fd, &frame);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to send WebSocket frame: %s", esp_err_to_name(err));
        }
    }

    free(message);
}

bool web_server_send_text_async(const char *text)
{
    if (text == NULL || s_http_server == NULL) {
        return false;
    }

    char *copy = strdup(text);
    if (copy == NULL) {
        return false;
    }

    if (httpd_queue_work(s_http_server, ws_send_work, copy) != ESP_OK) {
        free(copy);
        return false;
    }

    return true;
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        s_active_ws_fd = httpd_req_to_sockfd(req);
        ESP_LOGI(TAG, "WebSocket client connected on fd=%d", s_active_ws_fd);
        return ESP_OK;
    }

    httpd_ws_frame_t frame = {0};
    frame.type = HTTPD_WS_TYPE_TEXT;

    esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
    if (err != ESP_OK) {
        return err;
    }

    if (frame.len > 0 && frame.len < 512) {
        frame.payload = calloc(1, frame.len + 1);
        if (frame.payload == NULL) {
            return ESP_ERR_NO_MEM;
        }

        err = httpd_ws_recv_frame(req, &frame, frame.len);
        if (err == ESP_OK) {
            ((char *)frame.payload)[frame.len] = '\0';
            ESP_LOGI(TAG, "WebSocket frame: %s", (char *)frame.payload);
        }

        free(frame.payload);
    }

    return ESP_OK;
}

static void web_server_task(void *arg)
{
    (void)arg;

    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "storage",
        .max_files = 8,
        .format_if_mount_failed = true,
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(ret));
        vTaskDelete(NULL);
        return;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 10240;
    config.core_id = 1;
    config.uri_match_fn = httpd_uri_match_wildcard;

    if (httpd_start(&s_http_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        vTaskDelete(NULL);
        return;
    }

    httpd_uri_t static_uri = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = static_handler,
        .user_ctx = NULL,
    };

    httpd_uri_t ws_uri = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = ws_handler,
        .user_ctx = NULL,
        .is_websocket = true,
    };

    httpd_register_uri_handler(s_http_server, &ws_uri);
    httpd_register_uri_handler(s_http_server, &static_uri);

    ESP_LOGI(TAG, "HTTP server started on core 1");

    vTaskDelete(NULL);
}

void web_server_init(void)
{
    xTaskCreatePinnedToCore(web_server_task, "web_server", 10240, NULL, 5, NULL, 1);
}
