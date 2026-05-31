#pragma once

#include <stdint.h>

#include "esp_err.h"

esp_err_t mpu6050_init(void);
esp_err_t mpu6050_read_accel(int16_t *acc_x, int16_t *acc_z);
