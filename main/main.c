#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_rom_sys.h"

#define I2C_PORT I2C_NUM_0
#define I2C_SDA_GPIO 8
#define I2C_SCL_GPIO 9
#define I2C_FREQ_HZ 400000
#define OLED_I2C_ADDR 0x3C
#define ADS1115_I2C_ADDR 0x48
#define DS18B20_GPIO 10

#define OLED_WIDTH 64
#define OLED_HEIGHT 32
#define OLED_PAGES (OLED_HEIGHT / 8)

#define DS18B20_CMD_SKIP_ROM 0xCC
#define DS18B20_CMD_CONVERT_T 0x44
#define DS18B20_CMD_READ_SCRATCHPAD 0xBE
#define DS18B20_SCRATCHPAD_SIZE 9
#define DS18B20_CONV_TIME_MS 750

static const char *TAG_OLED = "oled";
static const char *TAG_TEMP = "temp";
static const char *TAG_I2C = "i2c";

static i2c_master_bus_handle_t s_i2c_bus;
static SemaphoreHandle_t s_i2c_mutex;
static SemaphoreHandle_t s_temp_mutex;

typedef struct {
	i2c_master_bus_handle_t bus;
	i2c_master_dev_handle_t dev;
} oled_i2c_t;

typedef struct {
	i2c_master_dev_handle_t dev;
} ads1115_t;

static oled_i2c_t s_oled;
static ads1115_t s_ads;

static uint8_t s_framebuffer[OLED_WIDTH * OLED_PAGES];

typedef struct {
	float temp_c;
	float temp_f;
	bool valid;
} temp_state_t;

static temp_state_t s_temp_state;

static bool i2c_lock(TickType_t timeout_ticks)
{
	return xSemaphoreTake(s_i2c_mutex, timeout_ticks) == pdTRUE;
}

static void i2c_unlock(void)
{
	xSemaphoreGive(s_i2c_mutex);
}

static void set_pixel(uint8_t *buf, int x, int y, bool on)
{
	if (x < 0 || x >= OLED_WIDTH || y < 0 || y >= OLED_HEIGHT) {
		return;
	}
	size_t index = (y / 8) * OLED_WIDTH + x;
	uint8_t mask = 1 << (y % 8);
	if (on) {
		buf[index] |= mask;
	} else {
		buf[index] &= (uint8_t)~mask;
	}
}

typedef struct {
	char ch;
	uint8_t rows[7];
} glyph_t;

static const glyph_t s_font[] = {
	{' ', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
	{'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0x06}},
	{'-', {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}},
	{':', {0x00, 0x06, 0x06, 0x00, 0x06, 0x06, 0x00}},
	{'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}},
	{'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
	{'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}},
	{'3', {0x0E, 0x11, 0x01, 0x06, 0x01, 0x11, 0x0E}},
	{'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}},
	{'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},
	{'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}},
	{'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
	{'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}},
	{'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
	{'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}},
	{'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
};

static const glyph_t *find_glyph(char ch)
{
	for (size_t i = 0; i < sizeof(s_font) / sizeof(s_font[0]); i++) {
		if (s_font[i].ch == ch) {
			return &s_font[i];
		}
	}
	return &s_font[0];
}

static void draw_char(uint8_t *buf, int x, int y, char ch)
{
	const glyph_t *glyph = find_glyph(ch);
	for (int row = 0; row < 7; row++) {
		uint8_t row_bits = glyph->rows[row];
		for (int col = 0; col < 5; col++) {
			bool on = (row_bits & (1 << (4 - col))) != 0;
			set_pixel(buf, x + col, y + row, on);
		}
	}
}

static void draw_text(uint8_t *buf, int x, int y, const char *text)
{
	int cursor_x = x;
	while (*text != '\0') {
		draw_char(buf, cursor_x, y, *text);
		cursor_x += 6;
		text++;
	}
}

static esp_err_t oled_write_cmd(oled_i2c_t *oled, uint8_t cmd)
{
	uint8_t data[2] = {0x00, cmd};
	return i2c_master_transmit(oled->dev, data, sizeof(data), -1);
}

static esp_err_t oled_write_data(oled_i2c_t *oled, const uint8_t *data, size_t len)
{
	uint8_t payload[1 + OLED_WIDTH];
	if (len > OLED_WIDTH) {
		return ESP_ERR_INVALID_SIZE;
	}
	payload[0] = 0x40;
	memcpy(&payload[1], data, len);
	return i2c_master_transmit(oled->dev, payload, len + 1, -1);
}

static esp_err_t oled_init_display(oled_i2c_t *oled)
{
	ESP_RETURN_ON_ERROR(oled_write_cmd(oled, 0xAE), TAG_OLED, "display off failed");
	ESP_RETURN_ON_ERROR(oled_write_cmd(oled, 0x00), TAG_OLED, "set column low failed");
	ESP_RETURN_ON_ERROR(oled_write_cmd(oled, 0x12), TAG_OLED, "set column high failed");
	ESP_RETURN_ON_ERROR(oled_write_cmd(oled, 0x00), TAG_OLED, "set start line failed");
	ESP_RETURN_ON_ERROR(oled_write_cmd(oled, 0xB0), TAG_OLED, "set page addr failed");
	ESP_RETURN_ON_ERROR(oled_write_cmd(oled, 0x81), TAG_OLED, "contrast control failed");
	ESP_RETURN_ON_ERROR(oled_write_cmd(oled, 0x4F), TAG_OLED, "contrast value failed");
	ESP_RETURN_ON_ERROR(oled_write_cmd(oled, 0xA1), TAG_OLED, "segment remap failed");
	ESP_RETURN_ON_ERROR(oled_write_cmd(oled, 0xA6), TAG_OLED, "normal display failed");
	ESP_RETURN_ON_ERROR(oled_write_cmd(oled, 0xA8), TAG_OLED, "multiplex ratio failed");
	ESP_RETURN_ON_ERROR(oled_write_cmd(oled, 0x1F), TAG_OLED, "multiplex value failed");
	ESP_RETURN_ON_ERROR(oled_write_cmd(oled, 0xC8), TAG_OLED, "com scan dir failed");
	ESP_RETURN_ON_ERROR(oled_write_cmd(oled, 0xD3), TAG_OLED, "display offset failed");
	ESP_RETURN_ON_ERROR(oled_write_cmd(oled, 0x00), TAG_OLED, "display offset value failed");
	ESP_RETURN_ON_ERROR(oled_write_cmd(oled, 0x20), TAG_OLED, "addressing mode failed");
	ESP_RETURN_ON_ERROR(oled_write_cmd(oled, 0x01), TAG_OLED, "vertical addr mode failed");
	ESP_RETURN_ON_ERROR(oled_write_cmd(oled, 0xD5), TAG_OLED, "osc division failed");
	ESP_RETURN_ON_ERROR(oled_write_cmd(oled, 0x80), TAG_OLED, "osc division value failed");
	ESP_RETURN_ON_ERROR(oled_write_cmd(oled, 0xD9), TAG_OLED, "pre-charge period failed");
	ESP_RETURN_ON_ERROR(oled_write_cmd(oled, 0xF1), TAG_OLED, "pre-charge value failed");
	ESP_RETURN_ON_ERROR(oled_write_cmd(oled, 0xDA), TAG_OLED, "set com pins failed");
	ESP_RETURN_ON_ERROR(oled_write_cmd(oled, 0x12), TAG_OLED, "com pins value failed");
	ESP_RETURN_ON_ERROR(oled_write_cmd(oled, 0xDB), TAG_OLED, "vcomh set failed");
	ESP_RETURN_ON_ERROR(oled_write_cmd(oled, 0x40), TAG_OLED, "vcomh value failed");
	ESP_RETURN_ON_ERROR(oled_write_cmd(oled, 0x8D), TAG_OLED, "charge pump enable failed");
	ESP_RETURN_ON_ERROR(oled_write_cmd(oled, 0x14), TAG_OLED, "charge pump value failed");
	ESP_RETURN_ON_ERROR(oled_write_cmd(oled, 0xAF), TAG_OLED, "display on failed");
	return ESP_OK;
}

static esp_err_t oled_show(oled_i2c_t *oled, const uint8_t *buf)
{
	for (int page = 0; page < OLED_PAGES; page++) {
		ESP_RETURN_ON_ERROR(oled_write_cmd(oled, 0x22), TAG_OLED, "set page range failed");
		ESP_RETURN_ON_ERROR(oled_write_cmd(oled, page), TAG_OLED, "set page start failed");
		ESP_RETURN_ON_ERROR(oled_write_cmd(oled, page), TAG_OLED, "set page end failed");
		ESP_RETURN_ON_ERROR(oled_write_cmd(oled, 0x21), TAG_OLED, "set column range failed");
		ESP_RETURN_ON_ERROR(oled_write_cmd(oled, 0x20), TAG_OLED, "set column low failed");
		ESP_RETURN_ON_ERROR(oled_write_cmd(oled, 0x5F), TAG_OLED, "set column high failed");
		ESP_RETURN_ON_ERROR(oled_write_data(oled, &buf[page * OLED_WIDTH], OLED_WIDTH), TAG_OLED,
				"write data failed");
	}
	return ESP_OK;
}

static void ds18b20_drive_low(void)
{
	gpio_set_level(DS18B20_GPIO, 0);
}

static void ds18b20_release_bus(void)
{
	gpio_set_level(DS18B20_GPIO, 1);
}

static bool ds18b20_read_bus(void)
{
	return gpio_get_level(DS18B20_GPIO) != 0;
}

static bool ds18b20_reset_pulse(void)
{
	ds18b20_drive_low();
	esp_rom_delay_us(480);
	ds18b20_release_bus();
	esp_rom_delay_us(70);
	bool presence = !ds18b20_read_bus();
	esp_rom_delay_us(410);
	return presence;
}

static void ds18b20_write_bit(bool bit)
{
	if (bit) {
		ds18b20_drive_low();
		esp_rom_delay_us(6);
		ds18b20_release_bus();
		esp_rom_delay_us(64);
	} else {
		ds18b20_drive_low();
		esp_rom_delay_us(60);
		ds18b20_release_bus();
		esp_rom_delay_us(10);
	}
}

static bool ds18b20_read_bit(void)
{
	bool bit;
	
	ds18b20_drive_low();
	esp_rom_delay_us(6);
	ds18b20_release_bus();
	esp_rom_delay_us(9);
	bit = ds18b20_read_bus();
	esp_rom_delay_us(55);
	return bit;
}

static void ds18b20_write_byte(uint8_t value)
{
	for (int i = 0; i < 8; i++) {
		ds18b20_write_bit((value >> i) & 0x01);
	}
}

static uint8_t ds18b20_read_byte(void)
{
	uint8_t value = 0;
	for (int i = 0; i < 8; i++) {
		if (ds18b20_read_bit()) {
			value |= (1 << i);
		}
	}
	return value;
}

static uint8_t ds18b20_crc8(const uint8_t *data, size_t len)
{
	uint8_t crc = 0;

	for (size_t i = 0; i < len; i++) {
		uint8_t inbyte = data[i];
		for (int bit = 0; bit < 8; bit++) {
			uint8_t mix = (crc ^ inbyte) & 0x01;
			crc >>= 1;
			if (mix) {
				crc ^= 0x8C;
			}
			inbyte >>= 1;
		}
	}

	return crc;
}

static bool ds18b20_start_conversion(void)
{
	if (!ds18b20_reset_pulse()) {
		return false;
	}
	ds18b20_write_byte(DS18B20_CMD_SKIP_ROM);
	ds18b20_write_byte(DS18B20_CMD_CONVERT_T);
	return true;
}

static bool ds18b20_read_scratchpad(uint8_t *scratchpad)
{
	if (!ds18b20_reset_pulse()) {
		return false;
	}
	ds18b20_write_byte(DS18B20_CMD_SKIP_ROM);
	ds18b20_write_byte(DS18B20_CMD_READ_SCRATCHPAD);
	for (int i = 0; i < DS18B20_SCRATCHPAD_SIZE; i++) {
		scratchpad[i] = ds18b20_read_byte();
	}
	return ds18b20_crc8(scratchpad, DS18B20_SCRATCHPAD_SIZE - 1) == scratchpad[DS18B20_SCRATCHPAD_SIZE - 1];
}

static bool ds18b20_read_temperature(float *out_c)
{
	uint8_t scratchpad[DS18B20_SCRATCHPAD_SIZE] = {0};
	if (!ds18b20_read_scratchpad(scratchpad)) {
		return false;
	}

	int16_t raw = (int16_t)((scratchpad[1] << 8) | scratchpad[0]);
	*out_c = (float)raw / 16.0f;
	return true;
}

static void format_temp_line(char *out, size_t len, char label, float value, bool valid)
{
	if (valid) {
		snprintf(out, len, "%c:%5.1f", label, (double)value);
	} else {
		snprintf(out, len, "%c: --.-", label);
	}
}

static void oled_task(void *arg)
{
	(void)arg;
	char line_c[12];
	char line_f[12];

	ESP_LOGI(TAG_OLED, "OLED temperature display running");

	while (true) {
		temp_state_t snapshot = {.valid = false, .temp_c = 0.0f, .temp_f = 0.0f};
		if (xSemaphoreTake(s_temp_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
			snapshot = s_temp_state;
			xSemaphoreGive(s_temp_mutex);
		}

		format_temp_line(line_c, sizeof(line_c), 'C', snapshot.temp_c, snapshot.valid);
		format_temp_line(line_f, sizeof(line_f), 'F', snapshot.temp_f, snapshot.valid);

		memset(s_framebuffer, 0x00, sizeof(s_framebuffer));
		draw_text(s_framebuffer, 0, 0, line_c);
		draw_text(s_framebuffer, 0, 16, line_f);

		if (i2c_lock(pdMS_TO_TICKS(100))) {
			esp_err_t err = oled_show(&s_oled, s_framebuffer);
			i2c_unlock();
			if (err != ESP_OK) {
				ESP_LOGW(TAG_OLED, "OLED update failed: %s", esp_err_to_name(err));
			}
		} else {
			ESP_LOGW(TAG_I2C, "I2C mutex timeout (OLED)");
		}

		vTaskDelay(pdMS_TO_TICKS(250));
	}
}

static void temp_task(void *arg)
{
	(void)arg;
	TickType_t last_wake = xTaskGetTickCount();

	while (true) {
		bool ok = ds18b20_start_conversion();
		if (!ok) {
			if (xSemaphoreTake(s_temp_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
				s_temp_state.valid = false;
				xSemaphoreGive(s_temp_mutex);
			}
			vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(DS18B20_CONV_TIME_MS));
			continue;
		}

		vTaskDelay(pdMS_TO_TICKS(DS18B20_CONV_TIME_MS));

		float temp_c = 0.0f;
		ok = ds18b20_read_temperature(&temp_c);
		if (xSemaphoreTake(s_temp_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
			if (ok) {
				s_temp_state.temp_c = temp_c;
				s_temp_state.temp_f = (temp_c * 9.0f / 5.0f) + 32.0f;
				s_temp_state.valid = true;
			} else {
				s_temp_state.valid = false;
			}
			xSemaphoreGive(s_temp_mutex);
		}

		vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(DS18B20_CONV_TIME_MS));
	}
}

void app_main(void)
{
	s_i2c_mutex = xSemaphoreCreateMutex();
	if (s_i2c_mutex == NULL) {
		ESP_LOGE(TAG_I2C, "Failed to create I2C mutex");
		return;
	}

	s_temp_mutex = xSemaphoreCreateMutex();
	if (s_temp_mutex == NULL) {
		ESP_LOGE(TAG_TEMP, "Failed to create temperature mutex");
		return;
	}

	gpio_config_t ds18b20_gpio = {
		.pin_bit_mask = 1ULL << DS18B20_GPIO,
		.mode = GPIO_MODE_INPUT_OUTPUT_OD,
		.pull_up_en = GPIO_PULLUP_ENABLE,
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
		.intr_type = GPIO_INTR_DISABLE,
	};
	ESP_ERROR_CHECK(gpio_config(&ds18b20_gpio));
	ds18b20_release_bus();

	i2c_master_bus_config_t bus_config = {
		.clk_source = I2C_CLK_SRC_DEFAULT,
		.i2c_port = I2C_PORT,
		.scl_io_num = I2C_SCL_GPIO,
		.sda_io_num = I2C_SDA_GPIO,
		.glitch_ignore_cnt = 7,
		.flags.enable_internal_pullup = true,
	};
	ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &s_i2c_bus));
	s_oled.bus = s_i2c_bus;

	i2c_device_config_t dev_config = {
		.dev_addr_length = I2C_ADDR_BIT_LEN_7,
		.device_address = OLED_I2C_ADDR,
		.scl_speed_hz = I2C_FREQ_HZ,
	};
	ESP_ERROR_CHECK(i2c_master_bus_add_device(s_oled.bus, &dev_config, &s_oled.dev));

	i2c_device_config_t ads_config = {
		.dev_addr_length = I2C_ADDR_BIT_LEN_7,
		.device_address = ADS1115_I2C_ADDR,
		.scl_speed_hz = I2C_FREQ_HZ,
	};
	ESP_ERROR_CHECK(i2c_master_bus_add_device(s_i2c_bus, &ads_config, &s_ads.dev));

	if (i2c_lock(pdMS_TO_TICKS(200))) {
		esp_err_t err = oled_init_display(&s_oled);
		if (err != ESP_OK) {
			ESP_LOGE(TAG_OLED, "OLED init failed: %s", esp_err_to_name(err));
		}
		i2c_unlock();
	} else {
		ESP_LOGE(TAG_I2C, "I2C mutex timeout during init");
		return;
	}

	xTaskCreate(oled_task, "oled_task", 4096, NULL, 5, NULL);
	xTaskCreate(temp_task, "temp_task", 4096, NULL, 5, NULL);
}
