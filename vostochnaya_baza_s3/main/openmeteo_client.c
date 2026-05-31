#include "openmeteo_client.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "METEO_CLIENT";
static int meteo_cache_idx = 0;

struct fetch_ctx {
    char *buffer;
    int len;
};

static esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
    struct fetch_ctx *ctx = (struct fetch_ctx *)evt->user_data;
    switch(evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (ctx->buffer == NULL) {
                ctx->buffer = malloc(evt->data_len + 1);
                ctx->len = 0;
            } else {
                char *new_buf = realloc(ctx->buffer, ctx->len + evt->data_len + 1);
                if (new_buf) {
                    ctx->buffer = new_buf;
                } else {
                    ESP_LOGE(TAG, "Failed to allocate memory");
                    return ESP_FAIL;
                }
            }
            if (ctx->buffer) {
                memcpy(ctx->buffer + ctx->len, evt->data, evt->data_len);
                ctx->len += evt->data_len;
                ctx->buffer[ctx->len] = 0;
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

static char* fetch_async(const char* url) {
    struct fetch_ctx ctx = {0};
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = _http_event_handler,
        .user_data = &ctx,
        .is_async = true,
        .timeout_ms = 15000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return NULL;

    esp_err_t err;
    while (1) {
        err = esp_http_client_perform(client);
        if (err != ESP_ERR_HTTP_EAGAIN) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    esp_http_client_cleanup(client);

    if (err == ESP_OK && ctx.buffer) {
        return ctx.buffer;
    } else {
        if (ctx.buffer) free(ctx.buffer);
        return NULL;
    }
}

bool fetch_and_cache_weather(double latitude, double longitude) {
    char url[256];
    snprintf(url, sizeof(url), "http://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f&current_weather=true", latitude, longitude);
    ESP_LOGI(TAG, "Fetching: %s", url);
    char *data = fetch_async(url);
    if (!data) return false;

    cJSON *json = cJSON_Parse(data);
    if (json) {
        char *str = cJSON_PrintUnformatted(json);
        if (str) {
            char filename[64];
            snprintf(filename, sizeof(filename), "/sdcard/meteo_cache_%d.json", meteo_cache_idx);
            FILE *f = fopen(filename, "w");
            if (f) {
                fprintf(f, "%s", str);
                fclose(f);
                ESP_LOGI(TAG, "Saved weather to %s", filename);
            } else {
                ESP_LOGE(TAG, "Failed to open %s for writing", filename);
            }
            meteo_cache_idx = (meteo_cache_idx + 1) % 10;
            cJSON_free(str);
        }
        cJSON_Delete(json);
    } else {
        ESP_LOGE(TAG, "Failed to parse JSON");
    }
    free(data);
    return true;
}

bool fetch_and_cache_wind_profile(double latitude, double longitude) {
    char url[256];
    snprintf(url, sizeof(url), "http://api.sondehub.org/tawhiri?profile=standard&lat=%.4f&lon=%.4f", latitude, longitude);
    ESP_LOGI(TAG, "Fetching wind profile: %s", url);
    char *data = fetch_async(url);
    if (!data) return false;

    cJSON *json = cJSON_Parse(data);
    if (json) {
        char *str = cJSON_PrintUnformatted(json);
        if (str) {
            FILE *f = fopen("/sdcard/wind_profile.json", "w");
            if (f) {
                fprintf(f, "%s", str);
                fclose(f);
                ESP_LOGI(TAG, "Saved wind profile to /sdcard/wind_profile.json");
            } else {
                ESP_LOGE(TAG, "Failed to open /sdcard/wind_profile.json for writing");
            }
            cJSON_free(str);
        }
        cJSON_Delete(json);
    } else {
        ESP_LOGE(TAG, "Failed to parse JSON");
    }
    free(data);
    return true;
}
