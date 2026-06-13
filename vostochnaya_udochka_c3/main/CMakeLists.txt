idf_component_register(
    SRCS
       
        "mpu6050.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        nvs_flash
        esp_wifi
        esp_event
        esp_netif
        espnow
        driver
        esp_adc)
