#include "metar_fetcher.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gps_parser.h"
#include "network_watchdog.h"
#include "web_server.h"

#ifndef CONFIG_VOSTOK_AVWX_TOKEN
#define CONFIG_VOSTOK_AVWX_TOKEN "REPLACE_WITH_AVWX_TOKEN"
#endif

static const char *TAG = "METAR";

typedef struct {
    char *buffer;
    size_t length;
    size_t capacity;
    bool success;
} metar_session_t;

static TaskHandle_t s_metar_task;
static volatile bool s_fetch_now;

static bool metar_session_append(metar_session_t *session, const char *data, size_t data_len)
{
    size_t required = session->length + data_len + 1;
    if (required > session->capacity) {
        size_t new_capacity = session->capacity == 0 ? 1024 : session->capacity;
        while (new_capacity < required) {
            new_capacity *= 2;
        }

        char *new_buffer = realloc(session->buffer, new_capacity);
        if (new_buffer == NULL) {
            return false;
        }

        session->buffer = new_buffer;
        session->capacity = new_capacity;
    }

    memcpy(session->buffer + session->length, data, data_len);
    session->length += data_len;
    session->buffer[session->length] = '\0';
    return true;
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    metar_session_t *session = evt->user_data;

    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (session != NULL && evt->data != NULL && evt->data_len > 0) {
                if (!metar_session_append(session, evt->data, evt->data_len)) {
                    return ESP_ERR_NO_MEM;
                }
            }
            break;
        case HTTP_EVENT_ON_FINISH:
            if (session != NULL && session->buffer != NULL) {
                cJSON *root = cJSON_ParseWithLength(session->buffer, session->length);
                if (root != NULL) {
                    cJSON *result = cJSON_CreateObject();
                    if (result != NULL) {
                        cJSON_AddStringToObject(result, "type", "metar");
                        cJSON_AddStringToObject(result, "raw", session->buffer);

                        cJSON *station = cJSON_GetObjectItemCaseSensitive(root, "station");
                        if (cJSON_IsString(station) && station->valuestring != NULL) {
                            cJSON_AddStringToObject(result, "station", station->valuestring);
                        }

                        char *json = cJSON_PrintUnformatted(result);
                        if (json != NULL) {
                            web_server_send_text_async(json);
                            free(json);
                            session->success = true;
                        }
                        cJSON_Delete(result);
                    }
                    cJSON_Delete(root);
                }
            }
            break;
        default:
            break;
    }

    return ESP_OK;
}

static void metar_session_free(metar_session_t *session)
{
    free(session->buffer);
    session->buffer = NULL;
    session->length = 0;
    session->capacity = 0;
}

static void metar_fetch_once(void)
{
    double latitude = 0.0;
    double longitude = 0.0;
    char utc[16] = {0};

    if (!gps_parser_get_fix(&latitude, &longitude, utc, sizeof(utc))) {
        ESP_LOGW(TAG, "Skipping METAR fetch because GPS fix is unavailable");
        return;
    }

    char url[256];
    snprintf(url, sizeof(url), "https://avwx.rest/api/metar/coord?lat=%.6f&lon=%.6f", latitude, longitude);

    metar_session_t session = {0};

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = &session,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to allocate HTTP client");
        metar_session_free(&session);
        return;
    }

    char auth_header[192];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", CONFIG_VOSTOK_AVWX_TOKEN);
    esp_http_client_set_header(client, "Authorization", auth_header);

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK && session.success) {
        ESP_LOGI(TAG, "METAR fetch completed successfully");
        network_watchdog_feed();
    } else {
        ESP_LOGE(TAG, "METAR fetch failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    metar_session_free(&session);
}

static void metar_task(void *arg)
{
    (void)arg;

    while (1) {
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10 * 60 * 1000)) > 0 || s_fetch_now) {
            s_fetch_now = false;
            metar_fetch_once();
        } else {
            metar_fetch_once();
        }
    }
}

void metar_fetcher_init(void)
{
    xTaskCreate(metar_task, "metar_task", 6144, NULL, 5, &s_metar_task);
}

void metar_fetcher_request_now(void)
{
    s_fetch_now = true;
    if (s_metar_task != NULL) {
        xTaskNotifyGive(s_metar_task);
    }
}
