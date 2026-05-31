#include "mpu6050.h"

#include <stddef.h>

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#define MPU6050_I2C_PORT I2C_NUM_0
#define MPU6050_I2C_SDA GPIO_NUM_8
#define MPU6050_I2C_SCL GPIO_NUM_9
#define MPU6050_I2C_FREQ_HZ 400000
#define MPU6050_I2C_ADDR 0x68

#define MPU6050_REG_SMPLRT_DIV 0x19
#define MPU6050_REG_CONFIG 0x1A
#define MPU6050_REG_GYRO_CONFIG 0x1B
#define MPU6050_REG_ACCEL_CONFIG 0x1C
#define MPU6050_REG_ACCEL_CONFIG2 0x1D
#define MPU6050_REG_MOT_THR 0x1F
#define MPU6050_REG_MOT_DUR 0x20
#define MPU6050_REG_INT_ENABLE 0x38
#define MPU6050_REG_PWR_MGMT_1 0x6B
#define MPU6050_REG_ACCEL_XOUT_H 0x3B

static esp_err_t mpu6050_write_reg(uint8_t reg, uint8_t value)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if (cmd == NULL) {
        return ESP_ERR_NO_MEM;
    }

    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (MPU6050_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_write_byte(cmd, value, true);
    i2c_master_stop(cmd);

    esp_err_t err = i2c_master_cmd_begin(MPU6050_I2C_PORT, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return err;
}

static esp_err_t mpu6050_read_regs(uint8_t reg, uint8_t *data, size_t len)
{
    if (data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if (cmd == NULL) {
        return ESP_ERR_NO_MEM;
    }

    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (MPU6050_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (MPU6050_I2C_ADDR << 1) | I2C_MASTER_READ, true);
    if (len > 1) {
        i2c_master_read(cmd, data, len - 1, I2C_MASTER_ACK);
    }
    i2c_master_read_byte(cmd, data + len - 1, I2C_MASTER_NACK);
    i2c_master_stop(cmd);

    esp_err_t err = i2c_master_cmd_begin(MPU6050_I2C_PORT, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return err;
}

esp_err_t mpu6050_init(void)
{
    i2c_config_t config = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = MPU6050_I2C_SDA,
        .scl_io_num = MPU6050_I2C_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = MPU6050_I2C_FREQ_HZ,
    };

    ESP_ERROR_CHECK(i2c_param_config(MPU6050_I2C_PORT, &config));
    ESP_ERROR_CHECK(i2c_driver_install(MPU6050_I2C_PORT, config.mode, 0, 0, 0));

    ESP_ERROR_CHECK(mpu6050_write_reg(MPU6050_REG_PWR_MGMT_1, 0x00));
    ESP_ERROR_CHECK(mpu6050_write_reg(MPU6050_REG_SMPLRT_DIV, 0x07));
    ESP_ERROR_CHECK(mpu6050_write_reg(MPU6050_REG_CONFIG, 0x03));
    ESP_ERROR_CHECK(mpu6050_write_reg(MPU6050_REG_GYRO_CONFIG, 0x00));
    ESP_ERROR_CHECK(mpu6050_write_reg(MPU6050_REG_ACCEL_CONFIG, 0x00));
    ESP_ERROR_CHECK(mpu6050_write_reg(MPU6050_REG_ACCEL_CONFIG2, 0x03));
    ESP_ERROR_CHECK(mpu6050_write_reg(MPU6050_REG_MOT_THR, 10));
    ESP_ERROR_CHECK(mpu6050_write_reg(MPU6050_REG_MOT_DUR, 20));
    ESP_ERROR_CHECK(mpu6050_write_reg(MPU6050_REG_INT_ENABLE, 0x40));
    return ESP_OK;
}

esp_err_t mpu6050_read_accel(int16_t *acc_x, int16_t *acc_z)
{
    if (acc_x == NULL || acc_z == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t data[6] = {0};
    esp_err_t err = mpu6050_read_regs(MPU6050_REG_ACCEL_XOUT_H, data, sizeof(data));
    if (err != ESP_OK) {
        return err;
    }

    *acc_x = (int16_t)((data[0] << 8) | data[1]);
    *acc_z = (int16_t)((data[4] << 8) | data[5]);
    return ESP_OK;
}
