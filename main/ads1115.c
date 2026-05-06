#include "ads1115.h"
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include "esp_err.h"
#include "esp_log.h"

static const char *TAG = "ads1115";

// ADS1115 register pointers
#define ADS1115_REG_CONVERSION 0x00
#define ADS1115_REG_CONFIG     0x01
#define ADS1115_REG_LO_THRESH  0x02
#define ADS1115_REG_HI_THRESH  0x03

// Helper: write 16-bit config to config register
esp_err_t ads1115_write_config(i2c_master_dev_handle_t dev, uint16_t config)
{
    uint8_t buf[3];
    buf[0] = ADS1115_REG_CONFIG;
    buf[1] = (config >> 8) & 0xFF;
    buf[2] = config & 0xFF;
    return i2c_master_transmit(dev, buf, sizeof(buf), -1);
}

esp_err_t ads1115_read_config(i2c_master_dev_handle_t dev, uint16_t *out_config)
{
    uint8_t ptr = ADS1115_REG_CONFIG;
    esp_err_t err = i2c_master_transmit(dev, &ptr, 1, -1);
    if (err != ESP_OK) return err;
    uint8_t rbuf[2];
    err = i2c_master_receive(dev, rbuf, 2, -1);
    if (err != ESP_OK) return err;
    *out_config = (rbuf[0] << 8) | rbuf[1];
    return ESP_OK;
}

esp_err_t ads1115_read_conversion(i2c_master_dev_handle_t dev, int16_t *out_raw)
{
    uint8_t ptr = ADS1115_REG_CONVERSION;
    esp_err_t err = i2c_master_transmit(dev, &ptr, 1, -1);
    if (err != ESP_OK) return err;
    uint8_t rbuf[2];
    err = i2c_master_receive(dev, rbuf, 2, -1);
    if (err != ESP_OK) return err;
    *out_raw = (int16_t)((rbuf[0] << 8) | rbuf[1]);
    return ESP_OK;
}

esp_err_t ads1115_init(i2c_master_dev_handle_t dev)
{
    // No special init required for ADS1115 beyond I2C device registration
    (void)dev;
    ESP_LOGI(TAG, "ADS1115 init noop");
    return ESP_OK;
}

// channel: 0..3 single-ended
esp_err_t ads1115_read_single_shot(i2c_master_dev_handle_t dev, uint8_t channel, int16_t *out_raw)
{
    if (channel > 3) return ESP_ERR_INVALID_ARG;

    // Build config: OS=1 (start single), MUX bits for single-ended channel,
    // PGA = 0b001 (±4.096V), MODE=1 single-shot, DR=0b111 (860SPS), comparator disabled
    uint16_t cfg = 0;
    cfg |= (1U << 15); // OS = 1 -> start conversion

    // MUX[14:12] for single-ended: 100,101,110,111 for AIN0..AIN3
    uint16_t mux_bits = 0x4 + channel; // 4..7
    cfg |= (mux_bits & 0x7) << 12;

    cfg |= (0x1 << 9); // PGA = 0b001 -> ±4.096V
    cfg |= (1U << 8);  // MODE = single-shot
    cfg |= (0x7 << 5); // DR = 0b111 -> 860 SPS

    esp_err_t err = ads1115_write_config(dev, cfg);
    if (err != ESP_OK) return err;

    // Poll for conversion complete (OS bit becomes 1 when done)
    const int max_attempts = 100;
    for (int i = 0; i < max_attempts; i++) {
        uint16_t cur_cfg = 0;
        err = ads1115_read_config(dev, &cur_cfg);
        if (err != ESP_OK) return err;
        if (cur_cfg & 0x8000) {
            // conversion complete
            return ads1115_read_conversion(dev, out_raw);
        }
        // small delay
        usleep(1000); // 1ms
    }

    ESP_LOGW(TAG, "ADS1115 conversion timeout");
    return ESP_ERR_TIMEOUT;
}
