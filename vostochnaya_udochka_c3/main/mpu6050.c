#include "mpu6050.h"
#include "driver/i2c.h"

esp_err_t i2c_master_init(void) {
    int i2c_master_port = I2C_MASTER_NUM;
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
    };
    esp_err_t err = i2c_param_config(i2c_master_port, &conf);
    if (err != ESP_OK) return err;
    return i2c_driver_install(i2c_master_port, conf.mode, I2C_MASTER_RX_BUF_DISABLE, I2C_MASTER_TX_BUF_DISABLE, 0);
}

static esp_err_t mpu6050_write_byte(uint8_t reg_addr, uint8_t data) {
    uint8_t write_buf[2] = {reg_addr, data};
    return i2c_master_write_to_device(I2C_MASTER_NUM, MPU6050_ADDR, write_buf, sizeof(write_buf), I2C_MASTER_TIMEOUT_MS / portTICK_PERIOD_MS);
}

esp_err_t mpu6050_init(void) {
    esp_err_t err;

    // Wake up MPU6050
    err = mpu6050_write_byte(MPU6050_PWR_MGMT_1, 0x00);
    if (err != ESP_OK) return err;

    // Set accelerometer full scale range to +/- 2g
    err = mpu6050_write_byte(MPU6050_ACCEL_CONFIG, 0x00);
    if (err != ESP_OK) return err;

    // Configure interrupt pin (active high, push-pull, 50us pulse, clear on read)
    err = mpu6050_write_byte(MPU6050_INT_PIN_CFG, 0x00);
    if (err != ESP_OK) return err;

    // Enable motion interrupt
    err = mpu6050_write_byte(MPU6050_INT_ENABLE, 0x40);
    if (err != ESP_OK) return err;

    // Set motion detection threshold
    err = mpu6050_write_byte(MPU6050_MOT_THR, 2); // 2mg per LSB
    if (err != ESP_OK) return err;

    // Set motion detection duration
    err = mpu6050_write_byte(MPU6050_MOT_DUR, 1); // 1ms
    return err;
}

esp_err_t mpu6050_read_accel(int16_t *acc_x, int16_t *acc_z) {
    uint8_t data_x[2], data_z[2];
    esp_err_t err;

    err = i2c_master_write_read_device(I2C_MASTER_NUM, MPU6050_ADDR, (uint8_t[]){MPU6050_ACCEL_XOUT_H}, 1, data_x, 2, I2C_MASTER_TIMEOUT_MS / portTICK_PERIOD_MS);
    if (err != ESP_OK) return err;

    err = i2c_master_write_read_device(I2C_MASTER_NUM, MPU6050_ADDR, (uint8_t[]){MPU6050_ACCEL_ZOUT_H}, 1, data_z, 2, I2C_MASTER_TIMEOUT_MS / portTICK_PERIOD_MS);
    if (err != ESP_OK) return err;

    *acc_x = (data_x[0] << 8) | data_x[1];
    *acc_z = (data_z[0] << 8) | data_z[1];

    return ESP_OK;
}
