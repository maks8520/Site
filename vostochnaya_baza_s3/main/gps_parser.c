#include "gps_parser.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>
#include <math.h>

static const char *TAG = "GPS";

double gps_lat = 0.0;
double gps_lon = 0.0;
bool gps_has_fix = false;

void parse_nmea(const char* sentence) {
    if (strncmp(sentence, "$GPRMC", 6) == 0) {
        char *p = strdup(sentence);
        char *tok = strtok(p, ",");
        int field = 0;
        char time_str[20] = {0};
        char date_str[20] = {0};
        char status = 'V';
        char lat_str[20] = {0}, lon_str[20] = {0};
        char lat_dir = 'N', lon_dir = 'E';

        while(tok != NULL) {
            if (field == 1) strncpy(time_str, tok, sizeof(time_str)-1);
            else if (field == 2) status = tok[0];
            else if (field == 3) strncpy(lat_str, tok, sizeof(lat_str)-1);
            else if (field == 4) lat_dir = tok[0];
            else if (field == 5) strncpy(lon_str, tok, sizeof(lon_str)-1);
            else if (field == 6) lon_dir = tok[0];
            else if (field == 9) strncpy(date_str, tok, sizeof(date_str)-1);
            tok = strtok(NULL, ",");
            field++;
        }

        if (status == 'A' && strlen(time_str) >= 6 && strlen(date_str) == 6) {
            struct tm t = {0};
            char buf[5] = {0};

            strncpy(buf, time_str, 2); buf[2] = 0; t.tm_hour = atoi(buf);
            strncpy(buf, time_str+2, 2); buf[2] = 0; t.tm_min = atoi(buf);
            strncpy(buf, time_str+4, 2); buf[2] = 0; t.tm_sec = atoi(buf);

            strncpy(buf, date_str, 2); buf[2] = 0; t.tm_mday = atoi(buf);
            strncpy(buf, date_str+2, 2); buf[2] = 0; t.tm_mon = atoi(buf) - 1;
            strncpy(buf, date_str+4, 2); buf[2] = 0; t.tm_year = atoi(buf) + 100;

            putenv("TZ=UTC");
            tzset();
            time_t epoch = mktime(&t);
            struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
            settimeofday(&tv, NULL);
            ESP_LOGI(TAG, "Time synced via GPS: %s", asctime(&t));

            double lat = atof(lat_str);
            double lon = atof(lon_str);
            gps_lat = (int)(lat/100) + fmod(lat, 100.0)/60.0;
            if (lat_dir == 'S') gps_lat = -gps_lat;
            gps_lon = (int)(lon/100) + fmod(lon, 100.0)/60.0;
            if (lon_dir == 'W') gps_lon = -gps_lon;

            gps_has_fix = true;
        }
        free(p);
    }
}

static void gps_task(void *pvParameters) {
    while (1) {
        // Mock a NMEA sentence for demonstration
        parse_nmea("$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A");
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

void gps_parser_init(void) {
    xTaskCreate(gps_task, "gps_task", 4096, NULL, 5, NULL);
}
