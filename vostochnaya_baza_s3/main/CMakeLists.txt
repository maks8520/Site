idf_component_register(SRCS "main.c" "esp_now_receiver.c" "gps_parser.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES nvs_flash esp_wifi esp_now esp_http_server cjson fatfs spiffs)
