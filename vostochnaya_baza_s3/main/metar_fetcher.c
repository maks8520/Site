#include "metar_fetcher.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gps_parser.h"
#include "cJSON.h"
#include <math.h>

static const char *TAG = "METAR";

static double calculate_distance(double lat1, double lon1, double lat2, double lon2) {
    const double R = 6371.0;
    double dLat = (lat2 - lat1) * M_PI / 180.0;
    double dLon = (lon2 - lon1) * M_PI / 180.0;
    lat1 = lat1 * M_PI / 180.0;
    lat2 = lat2 * M_PI / 180.0;
    double a = sin(dLat/2) * sin(dLat/2) + sin(dLon/2) * sin(dLon/2) * cos(lat1) * cos(lat2);
    double c = 2 * atan2(sqrt(a), sqrt(1-a));
    return R * c;
}

esp_err_t _http_event_handle(esp_http_client_event_t *evt) {
    switch(evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (!esp_http_client_is_chunked_response(evt->client)) {
                // Ensure null-terminated string
                char* response_data = malloc(evt->data_len + 1);
                if (response_data) {
                    memcpy(response_data, evt->data, evt->data_len);
                    response_data[evt->data_len] = '\0';

                    double station_lat = 48.0; // Mock station coordinates
                    double station_lon = 11.0;
                    double dist = calculate_distance(gps_lat, gps_lon, station_lat, station_lon);

                    cJSON *root = cJSON_CreateObject();
                    cJSON_AddStringToObject(root, "metar_raw", response_data);
                    cJSON_AddNumberToObject(root, "distance_km", dist);
                    char *json_str = cJSON_PrintUnformatted(root);

                    ESP_LOGI(TAG, "WS METAR Payload: %s", json_str);

                    free(json_str);
                    cJSON_Delete(root);
                    free(response_data);
                }
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

static void metar_task(void *pvParameters) {
    while (1) {
        if (gps_has_fix) {
            char url[256];
            snprintf(url, sizeof(url), "https://avwx.rest/api/metar/coord?lat=%f&lon=%f", gps_lat, gps_lon);

            esp_http_client_config_t config = {
                .url = url,
                .event_handler = _http_event_handle,
                .crt_bundle_attach = NULL // Normally we use esp_crt_bundle_attach here
            };
            esp_http_client_handle_t client = esp_http_client_init(&config);
            esp_http_client_set_header(client, "Authorization", "Bearer MOCK_TOKEN"); // Placeholder

            esp_err_t err = esp_http_client_perform(client);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "METAR fetch successful");
            } else {
                ESP_LOGE(TAG, "METAR fetch failed: %s", esp_err_to_name(err));
            }
            esp_http_client_cleanup(client);
        }
        vTaskDelay(pdMS_TO_TICKS(10 * 60 * 1000));
    }
}

void metar_fetcher_init(void) {
    xTaskCreate(metar_task, "metar_task", 8192, NULL, 5, NULL);
}
