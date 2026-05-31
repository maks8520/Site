idf_component_register(SRCS "main.c" "esp_now_receiver.c" "gps_parser.c" "metar_fetcher.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES nvs_flash esp_wifi esp_http_server json fatfs spiffs esp_http_client)
