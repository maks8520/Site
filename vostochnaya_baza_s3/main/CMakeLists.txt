idf_component_register(SRCS "main.c" "meteo_task.c" "openmeteo_client.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_http_client json fatfs sdmmc esp_wifi)
