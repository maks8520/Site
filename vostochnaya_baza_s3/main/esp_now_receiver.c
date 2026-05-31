#include "esp_now_receiver.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "network_watchdog.h"
#include "web_server.h"

static const char *TAG = "ESP_NOW_RECV";

#define ESP_NOW_RX_QUEUE_DEPTH 8
#define ESP_NOW_MAX_PAYLOAD 250

typedef struct {
    uint8_t src_mac[ESP_NOW_ETH_ALEN];
    size_t len;
    char payload[ESP_NOW_MAX_PAYLOAD + 1];
} rx_item_t;

static QueueHandle_t s_rx_queue;

static void send_app_ack(const uint8_t *src_mac, uint8_t seq)
{
    char ack_payload[96];
    snprintf(ack_payload, sizeof(ack_payload), "{\"type\":\"ack\",\"seq\":%u,\"status\":\"ok\"}", seq);

    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, src_mac, ESP_NOW_ETH_ALEN);
    peer.ifidx = WIFI_IF_STA;
    peer.channel = 0;
    peer.encrypt = false;

    if (!esp_now_is_peer_exist(src_mac)) {
        esp_err_t peer_err = esp_now_add_peer(&peer);
        if (peer_err != ESP_OK && peer_err != ESP_ERR_ESPNOW_EXIST) {
            ESP_LOGW(TAG, "Failed to add peer for ACK: %s", esp_err_to_name(peer_err));
            return;
        }
    }

    esp_err_t err = esp_now_send(src_mac, (const uint8_t *)ack_payload, strlen(ack_payload));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to send application ACK: %s", esp_err_to_name(err));
    }
}

static void process_rx_item(const rx_item_t *item)
{
    cJSON *root = cJSON_ParseWithLength(item->payload, item->len);
    if (root == NULL) {
        ESP_LOGW(TAG, "Received non-JSON telemetry payload");
        return;
    }

    cJSON *telemetry = cJSON_GetObjectItemCaseSensitive(root, "telemetry");
    cJSON *seq = cJSON_GetObjectItemCaseSensitive(root, "seq");
    cJSON *battery = cJSON_GetObjectItemCaseSensitive(root, "battery_mv");

    cJSON *acc_x = telemetry != NULL ? cJSON_GetObjectItemCaseSensitive(telemetry, "acc_x") : NULL;
    cJSON *acc_z = telemetry != NULL ? cJSON_GetObjectItemCaseSensitive(telemetry, "acc_z") : NULL;
    cJSON *hall = telemetry != NULL ? cJSON_GetObjectItemCaseSensitive(telemetry, "hall") : NULL;

    if (!cJSON_IsNumber(acc_x) || !cJSON_IsNumber(acc_z) || !cJSON_IsNumber(hall)) {
        cJSON_Delete(root);
        ESP_LOGW(TAG, "Telemetry payload is missing required fields");
        return;
    }

    char mac_text[18];
    snprintf(mac_text, sizeof(mac_text), MACSTR, MAC2STR(item->src_mac));

    cJSON *ws_root = cJSON_CreateObject();
    if (ws_root != NULL) {
        cJSON_AddStringToObject(ws_root, "source_mac", mac_text);
        cJSON *ws_telemetry = cJSON_CreateObject();
        if (ws_telemetry != NULL) {
            cJSON_AddNumberToObject(ws_telemetry, "acc_x", acc_x->valueint);
            cJSON_AddNumberToObject(ws_telemetry, "acc_z", acc_z->valueint);
            cJSON_AddNumberToObject(ws_telemetry, "hall", hall->valueint);
            if (cJSON_IsNumber(battery)) {
                cJSON_AddNumberToObject(ws_telemetry, "battery_mv", battery->valueint);
            }
            cJSON_AddItemToObject(ws_root, "telemetry", ws_telemetry);
        }

        char *json = cJSON_PrintUnformatted(ws_root);
        if (json != NULL) {
            web_server_send_text_async(json);
            free(json);
        }

        cJSON_Delete(ws_root);
    }

    uint8_t ack_seq = cJSON_IsNumber(seq) ? (uint8_t)seq->valueint : 0;
    send_app_ack(item->src_mac, ack_seq);
    network_watchdog_feed();
    ESP_LOGI(TAG, "Telemetry received from %s", mac_text);
    cJSON_Delete(root);
}

static void rx_task(void *arg)
{
    (void)arg;

    rx_item_t item;
    while (1) {
        if (xQueueReceive(s_rx_queue, &item, portMAX_DELAY) == pdTRUE) {
            process_rx_item(&item);
        }
    }
}

static void esp_now_recv_cb(const esp_now_recv_info_t *esp_now_info, const uint8_t *data, int len)
{
    if (esp_now_info == NULL || data == NULL || len <= 0) {
        return;
    }

    if (len > ESP_NOW_MAX_PAYLOAD) {
        ESP_LOGW(TAG, "Dropping oversized ESP-NOW payload: %d bytes", len);
        return;
    }

    rx_item_t item = {0};
    memcpy(item.src_mac, esp_now_info->src_addr, ESP_NOW_ETH_ALEN);
    item.len = (size_t)len;
    memcpy(item.payload, data, (size_t)len);
    item.payload[len] = '\0';

    if (xQueueSend(s_rx_queue, &item, 0) != pdTRUE) {
        ESP_LOGW(TAG, "ESP-NOW RX queue full");
    }
}

void esp_now_receiver_init(void)
{
    s_rx_queue = xQueueCreate(ESP_NOW_RX_QUEUE_DEPTH, sizeof(rx_item_t));
    if (s_rx_queue == NULL) {
        ESP_LOGE(TAG, "Failed to allocate ESP-NOW queue");
        return;
    }

    esp_err_t err = esp_now_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ESP-NOW init failed: %s", esp_err_to_name(err));
        return;
    }

    esp_now_register_recv_cb(esp_now_recv_cb);
    xTaskCreate(rx_task, "esp_now_rx", 4096, NULL, 6, NULL);
    ESP_LOGI(TAG, "ESP-NOW receiver initialized");
}
