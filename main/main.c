#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"

#define I2C_PORT I2C_NUM_0
#define I2C_SDA_GPIO 8
#define I2C_SCL_GPIO 9
#define I2C_FREQ_HZ 400000
#define OLED_I2C_ADDR 0x3C

#define OLED_WIDTH 64
#define OLED_HEIGHT 32
#define OLED_PAGES (OLED_HEIGHT / 8)

static const char *TAG = "oled";

typedef struct {
	i2c_master_bus_handle_t bus;
	i2c_master_dev_handle_t dev;
} oled_i2c_t;

static uint8_t s_framebuffer[OLED_WIDTH * OLED_PAGES];

typedef struct {
	char ch;
	uint8_t rows[7];
} glyph_t;

static const glyph_t s_font[] = {
	{' ', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
	{'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
	{'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}},
	{'a', {0x00, 0x0E, 0x01, 0x0F, 0x11, 0x11, 0x0F}},
	{'e', {0x00, 0x0E, 0x11, 0x1F, 0x10, 0x11, 0x0E}},
	{'h', {0x10, 0x10, 0x10, 0x1E, 0x11, 0x11, 0x11}},
	{'k', {0x10, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}},
	{'l', {0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
	{'m', {0x00, 0x1A, 0x15, 0x15, 0x15, 0x15, 0x15}},
	{'r', {0x00, 0x16, 0x19, 0x10, 0x10, 0x10, 0x10}},
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

static void oled_init_display(oled_i2c_t *oled)
{
	oled_write_cmd(oled, 0xAE); // display off
	oled_write_cmd(oled, 0x00); // set lower column address
	oled_write_cmd(oled, 0x12); // set higher column address
	oled_write_cmd(oled, 0x00); // set display start line
	oled_write_cmd(oled, 0xB0); // set page address
	oled_write_cmd(oled, 0x81); // contrast control
	oled_write_cmd(oled, 0x4F);
	oled_write_cmd(oled, 0xA1); // segment remap
	oled_write_cmd(oled, 0xA6); // normal display
	oled_write_cmd(oled, 0xA8); // multiplex ratio
	oled_write_cmd(oled, 0x1F); // duty = 1/32
	oled_write_cmd(oled, 0xC8); // COM scan direction
	oled_write_cmd(oled, 0xD3); // display offset
	oled_write_cmd(oled, 0x00);
	oled_write_cmd(oled, 0x20);
	oled_write_cmd(oled, 0x01); // vertical addressing mode
	oled_write_cmd(oled, 0xD5); // osc division
	oled_write_cmd(oled, 0x80);
	oled_write_cmd(oled, 0xD9); // pre-charge period
	oled_write_cmd(oled, 0xF1);
	oled_write_cmd(oled, 0xDA); // set COM pins
	oled_write_cmd(oled, 0x12);
	oled_write_cmd(oled, 0xDB); // set VCOMH
	oled_write_cmd(oled, 0x40);
	oled_write_cmd(oled, 0x8D); // charge pump enable
	oled_write_cmd(oled, 0x14);
	oled_write_cmd(oled, 0xAF); // display ON
}

static void oled_show(oled_i2c_t *oled, const uint8_t *buf)
{
	for (int page = 0; page < OLED_PAGES; page++) {
		oled_write_cmd(oled, 0x22);
		oled_write_cmd(oled, page);
		oled_write_cmd(oled, page);
		oled_write_cmd(oled, 0x21);
		oled_write_cmd(oled, 0x20);
		oled_write_cmd(oled, 0x5F);
		oled_write_data(oled, &buf[page * OLED_WIDTH], OLED_WIDTH);
	}
}

void app_main(void)
{
	oled_i2c_t oled_i2c = {0};

	i2c_master_bus_config_t bus_config = {
		.clk_source = I2C_CLK_SRC_DEFAULT,
		.i2c_port = I2C_PORT,
		.scl_io_num = I2C_SCL_GPIO,
		.sda_io_num = I2C_SDA_GPIO,
		.glitch_ignore_cnt = 7,
		.flags.enable_internal_pullup = true,
	};
	ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &oled_i2c.bus));

	i2c_device_config_t dev_config = {
		.dev_addr_length = I2C_ADDR_BIT_LEN_7,
		.device_address = OLED_I2C_ADDR,
		.scl_speed_hz = I2C_FREQ_HZ,
	};
	ESP_ERROR_CHECK(i2c_master_bus_add_device(oled_i2c.bus, &dev_config, &oled_i2c.dev));

	oled_init_display(&oled_i2c);

	memset(s_framebuffer, 0x00, sizeof(s_framebuffer));
	draw_text(s_framebuffer, 0, 1, "Aalekh");
	draw_text(s_framebuffer, 0, 12, "Sharma");
	oled_show(&oled_i2c, s_framebuffer);

	ESP_LOGI(TAG, "OLED update done");

	while (true) {
		vTaskDelay(pdMS_TO_TICKS(1000));
	}
}
