idf_component_register(SRCS "main.c" "mpu6050.c"
                    INCLUDE_DIRS "."
                    REQUIRES nvs_flash esp_wifi driver esp_adc)
