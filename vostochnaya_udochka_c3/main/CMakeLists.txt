 feature/udochka-optimization-9862857180887165867
idf_component_register(SRCS "main.c" "mpu6050.c"
                    INCLUDE_DIRS "."
                    REQUIRES nvs_flash esp_wifi driver esp_adc)
=======
idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES nvs_flash esp_wifi driver)
 main
