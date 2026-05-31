esp-idf-setup-10902462744309710451
idf_component_register(SRCS "main.c" "esp_now_receiver.c" "gps_parser.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES nvs_flash esp_wifi esp_now esp_http_server cjson fatfs spiffs)

feature/baza-s3-core-4120926348719574766
idf_component_register(SRCS "main.c" "esp_now_receiver.c" "gps_parser.c"
                    INCLUDE_DIRS ".")

idf_component_register(SRCS "main.c" "meteo_task.c" "openmeteo_client.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_http_client json fatfs sdmmc esp_wifi)
main
 main
