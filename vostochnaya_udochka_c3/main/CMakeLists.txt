idf_component_register(
    SRCS
       
        "mpu6050.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        nvs_flash
        esp_wifi
        esp_event
        esp_netif
        driver
        esp_adc)
