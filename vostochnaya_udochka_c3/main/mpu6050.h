#ifndef MPU6050_H
#define MPU6050_H

#include <stdint.h>
#include "esp_err.h"

// I2C GPIO configuration
#define I2C_MASTER_SCL_IO           9
#define I2C_MASTER_SDA_IO           8
#define I2C_MASTER_NUM              0
#define I2C_MASTER_FREQ_HZ          400000
#define I2C_MASTER_TX_BUF_DISABLE   0
#define I2C_MASTER_RX_BUF_DISABLE   0
#define I2C_MASTER_TIMEOUT_MS       1000

// MPU6050 constants
#define MPU6050_ADDR                0x68
#define MPU6050_PWR_MGMT_1          0x6B
#define MPU6050_INT_ENABLE          0x38
#define MPU6050_INT_PIN_CFG         0x37
#define MPU6050_MOT_THR             0x1F
#define MPU6050_MOT_DUR             0x20
#define MPU6050_ACCEL_CONFIG        0x1C
#define MPU6050_ACCEL_XOUT_H        0x3B
#define MPU6050_ACCEL_ZOUT_H        0x3F

esp_err_t i2c_master_init(void);
esp_err_t mpu6050_init(void);
esp_err_t mpu6050_read_accel(int16_t *acc_x, int16_t *acc_z);

#endif // MPU6050_H
