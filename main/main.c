#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <dirent.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "lwip/ip4_addr.h"

#include "u8g2.h"

#include "ads1115.h"
#include "data_logger.h"

#define I2C_PORT I2C_NUM_0
#define I2C_SDA_GPIO 8
#define I2C_SCL_GPIO 9
#define I2C_FREQ_HZ 400000
#define OLED_I2C_ADDR 0x3C
#define ADS1115_I2C_ADDR 0x48
#define DS18B20_GPIO 10
#define NAV_BUTTON_GPIO 13
#define SELECT_BUTTON_GPIO 14

#define OLED_WIDTH 64
#define OLED_HEIGHT 32

#define DS18B20_CMD_SKIP_ROM 0xCC
#define DS18B20_CMD_CONVERT_T 0x44
#define DS18B20_CMD_READ_SCRATCHPAD 0xBE
#define DS18B20_SCRATCHPAD_SIZE 9
#define DS18B20_CONV_TIME_MS 750

#define BOOT_SPLASH_MS 2000
#define UI_REFRESH_MS 50
#define HEARTLINE_BASE_Y 24
#define HEARTLINE_HEIGHT 6

#define ADS1115_FS_V 4.096f
#define ADS1115_LSB (ADS1115_FS_V / 32768.0f)
#define ADS_AVG_SAMPLES 4
#define ACQ_SAMPLE_PERIOD_MS 50

#define WIFI_AP_SSID "PhysoKit"
#define WIFI_AP_PASS "Fit@2026"
#define WIFI_MAX_CONN 4

#define U8X8_I2C_BUFFER_SIZE 512

static const char *TAG_UI = "ui";
static const char *TAG_TEMP = "temp";
static const char *TAG_I2C = "i2c";
static const char *TAG_WIFI = "wifi";
static const char *TAG_HTTP = "http";

typedef struct {
	i2c_master_bus_handle_t bus;
	i2c_master_dev_handle_t dev;
} oled_i2c_t;

typedef struct {
	i2c_master_dev_handle_t dev;
} ads1115_t;

typedef struct {
	float temp_c;
	bool valid;
} temp_state_t;

typedef enum {
	UI_STATE_BOOT = 0,
	UI_STATE_MENU_ACQ,
	UI_STATE_MENU_WEB,
	UI_STATE_ACQ_RUNNING,
	UI_STATE_WEB_RUNNING,
	UI_STATE_SAVE_NOTICE,
} ui_state_t;

static i2c_master_bus_handle_t s_i2c_bus;
static oled_i2c_t s_oled;
static ads1115_t s_ads;
static SemaphoreHandle_t s_i2c_mutex;
static SemaphoreHandle_t s_temp_mutex;
static SemaphoreHandle_t s_acq_mutex;

static u8g2_t s_u8g2;
static uint8_t s_u8x8_buffer[U8X8_I2C_BUFFER_SIZE];
static size_t s_u8x8_buf_len;

static temp_state_t s_temp_state;
static volatile bool s_acq_running;
static uint64_t s_acq_start_ms;
static uint64_t s_save_notice_until_ms;
static FILE *s_log_file;
static char s_log_path[64];
static char s_saved_name[32];
static ui_state_t s_ui_state = UI_STATE_BOOT;
static int s_heart_phase;

static bool s_wifi_running;
static esp_netif_t *s_netif_ap;
static httpd_handle_t s_http;
static char s_ap_ip[16];

static bool i2c_lock(TickType_t timeout_ticks)
{
	return xSemaphoreTake(s_i2c_mutex, timeout_ticks) == pdTRUE;
}

static void i2c_unlock(void)
{
	xSemaphoreGive(s_i2c_mutex);
}

static uint8_t u8x8_byte_esp32_i2c(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr)
{
	(void)u8x8;
	switch (msg) {
	case U8X8_MSG_BYTE_INIT:
		return 1;
	case U8X8_MSG_BYTE_START_TRANSFER:
		s_u8x8_buf_len = 0;
		return 1;
	case U8X8_MSG_BYTE_SEND:
		if (s_u8x8_buf_len + arg_int > sizeof(s_u8x8_buffer)) {
			return 0;
		}
		memcpy(&s_u8x8_buffer[s_u8x8_buf_len], arg_ptr, arg_int);
		s_u8x8_buf_len += arg_int;
		return 1;
	case U8X8_MSG_BYTE_END_TRANSFER: {
		esp_err_t err;
		if (!i2c_lock(pdMS_TO_TICKS(100))) {
			return 0;
		}
		err = i2c_master_transmit(s_oled.dev, s_u8x8_buffer, s_u8x8_buf_len, -1);
		i2c_unlock();
		return err == ESP_OK;
	}
	default:
		return 0;
	}
}

static uint8_t u8x8_gpio_and_delay_esp32(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr)
{
	(void)u8x8;
	(void)arg_ptr;
	switch (msg) {
	case U8X8_MSG_DELAY_MILLI:
		vTaskDelay(pdMS_TO_TICKS(arg_int));
		break;
	case U8X8_MSG_DELAY_10MICRO:
		esp_rom_delay_us((uint32_t)arg_int * 10U);
		break;
	case U8X8_MSG_DELAY_100NANO:
		esp_rom_delay_us(1);
		break;
	default:
		break;
	}
	return 1;
}

static void ui_draw_centered(const char *line1, const char *line2)
{
	int y1 = 12;
	int y2 = 26;
	int w1 = u8g2_GetStrWidth(&s_u8g2, line1);
	int x1 = (OLED_WIDTH - w1) / 2;
	if (x1 < 0) {
		x1 = 0;
	}
	u8g2_DrawStr(&s_u8g2, x1, y1, line1);
	if (line2 != NULL) {
		int w2 = u8g2_GetStrWidth(&s_u8g2, line2);
		int x2 = (OLED_WIDTH - w2) / 2;
		if (x2 < 0) {
			x2 = 0;
		}
		u8g2_DrawStr(&s_u8g2, x2, y2, line2);
	}
}

static void ui_draw_heartline(int phase)
{
	int x = phase % OLED_WIDTH;
	u8g2_DrawHLine(&s_u8g2, 0, HEARTLINE_BASE_Y, OLED_WIDTH);
	u8g2_DrawVLine(&s_u8g2, x, HEARTLINE_BASE_Y - HEARTLINE_HEIGHT, HEARTLINE_HEIGHT);
}

static void ui_show_boot_splash(void)
{
	u8g2_ClearBuffer(&s_u8g2);
	ui_draw_centered("Booting Up", NULL);
	u8g2_SendBuffer(&s_u8g2);
	vTaskDelay(pdMS_TO_TICKS(BOOT_SPLASH_MS));
}

static bool button_pressed(gpio_num_t gpio)
{
	return gpio_get_level(gpio) == 0;
}

static const char *basename_ptr(const char *path)
{
	const char *slash = strrchr(path, '/');
	return slash ? slash + 1 : path;
}

static bool ads1115_read_channel_avg(uint8_t ch, float *out_v)
{
	int32_t sum = 0;
	for (int i = 0; i < ADS_AVG_SAMPLES; i++) {
		int16_t raw = 0;
		if (!i2c_lock(pdMS_TO_TICKS(100))) {
			return false;
		}
		esp_err_t err = ads1115_read_single_shot(s_ads.dev, ch, &raw);
		i2c_unlock();
		if (err != ESP_OK) {
			return false;
		}
		sum += raw;
		vTaskDelay(pdMS_TO_TICKS(1));
	}
	*out_v = ((float)sum / (float)ADS_AVG_SAMPLES) * ADS1115_LSB;
	return true;
}

static bool start_acquisition(void)
{
	char path[64];
	FILE *f = data_logger_create_unique_file(path, sizeof(path));
	if (f == NULL) {
		ESP_LOGW(TAG_UI, "Failed to create CSV file");
		return false;
	}
	if (xSemaphoreTake(s_acq_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
		s_log_file = f;
		strncpy(s_log_path, path, sizeof(s_log_path) - 1);
		s_log_path[sizeof(s_log_path) - 1] = '\0';
		xSemaphoreGive(s_acq_mutex);
	}
	s_acq_start_ms = (uint64_t)(esp_timer_get_time() / 1000);
	s_acq_running = true;
	ESP_LOGI(TAG_UI, "Acquisition started: %s", s_log_path);
	return true;
}

static void stop_acquisition(void)
{
	s_acq_running = false;
	if (xSemaphoreTake(s_acq_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
		if (s_log_file != NULL) {
			fclose(s_log_file);
			s_log_file = NULL;
		}
		xSemaphoreGive(s_acq_mutex);
	}
	const char *base = basename_ptr(s_log_path);
	snprintf(s_saved_name, sizeof(s_saved_name), "%s", base);
	s_save_notice_until_ms = (uint64_t)(esp_timer_get_time() / 1000) + 3000;
	ESP_LOGI(TAG_UI, "Acquisition stopped, saved %s", s_saved_name);
}

static esp_err_t http_root_get_handler(httpd_req_t *req)
{
	httpd_resp_set_type(req, "text/html");
	httpd_resp_sendstr_chunk(req, "<html><head><title>PhysoKit Files</title></head><body>");
	httpd_resp_sendstr_chunk(req, "<h3>CSV Files</h3><ul>");

	DIR *dir = opendir("/spiffs");
	if (dir != NULL) {
		struct dirent *ent;
		while ((ent = readdir(dir)) != NULL) {
			if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
				continue;
			}
			httpd_resp_sendstr_chunk(req, "<li><a href=\"/download?name=");
			httpd_resp_sendstr_chunk(req, ent->d_name);
			httpd_resp_sendstr_chunk(req, "\">");
			httpd_resp_sendstr_chunk(req, ent->d_name);
			httpd_resp_sendstr_chunk(req, "</a></li>");
		}
		closedir(dir);
	}

	httpd_resp_sendstr_chunk(req, "</ul></body></html>");
	httpd_resp_send_chunk(req, NULL, 0);
	return ESP_OK;
}

static esp_err_t http_download_get_handler(httpd_req_t *req)
{
	char query[64];
	char name[32];
	if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing query");
		return ESP_FAIL;
	}
	if (httpd_query_key_value(query, "name", name, sizeof(name)) != ESP_OK) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing name");
		return ESP_FAIL;
	}
	if (strstr(name, "..") != NULL || strchr(name, '/') != NULL) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid name");
		return ESP_FAIL;
	}

	char path[64];
	snprintf(path, sizeof(path), "/spiffs/%s", name);
	FILE *f = fopen(path, "r");
	if (f == NULL) {
		httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
		return ESP_FAIL;
	}

	httpd_resp_set_type(req, "text/csv");
	httpd_resp_set_hdr(req, "Content-Disposition", "attachment");

	char buf[256];
	size_t read_bytes;
	while ((read_bytes = fread(buf, 1, sizeof(buf), f)) > 0) {
		if (httpd_resp_send_chunk(req, buf, read_bytes) != ESP_OK) {
			fclose(f);
			httpd_resp_sendstr_chunk(req, NULL);
			return ESP_FAIL;
		}
	}
	fclose(f);
	httpd_resp_send_chunk(req, NULL, 0);
	return ESP_OK;
}

static esp_err_t http_server_start(void)
{
	if (s_http != NULL) {
		return ESP_OK;
	}
	httpd_config_t config = HTTPD_DEFAULT_CONFIG();
	config.uri_match_fn = httpd_uri_match_wildcard;
	ESP_LOGI(TAG_HTTP, "Starting HTTP server");
	if (httpd_start(&s_http, &config) != ESP_OK) {
		ESP_LOGW(TAG_HTTP, "Failed to start HTTP server");
		s_http = NULL;
		return ESP_FAIL;
	}

	static const httpd_uri_t root_uri = {
		.uri = "/",
		.method = HTTP_GET,
		.handler = http_root_get_handler,
		.user_ctx = NULL,
	};
	static const httpd_uri_t download_uri = {
		.uri = "/download",
		.method = HTTP_GET,
		.handler = http_download_get_handler,
		.user_ctx = NULL,
	};
	httpd_register_uri_handler(s_http, &root_uri);
	httpd_register_uri_handler(s_http, &download_uri);
	return ESP_OK;
}

static void http_server_stop(void)
{
	if (s_http != NULL) {
		httpd_stop(s_http);
		s_http = NULL;
	}
}

static esp_err_t wifi_ap_start(void)
{
	if (s_wifi_running) {
		return ESP_OK;
	}
	if (s_netif_ap == NULL) {
		s_netif_ap = esp_netif_create_default_wifi_ap();
	}

	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&cfg));
	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

	wifi_config_t ap_config = {0};
	strncpy((char *)ap_config.ap.ssid, WIFI_AP_SSID, sizeof(ap_config.ap.ssid) - 1);
	strncpy((char *)ap_config.ap.password, WIFI_AP_PASS, sizeof(ap_config.ap.password) - 1);
	ap_config.ap.ssid_len = strlen(WIFI_AP_SSID);
	ap_config.ap.channel = 1;
	ap_config.ap.max_connection = WIFI_MAX_CONN;
	if (strlen(WIFI_AP_PASS) < 8) {
		ap_config.ap.authmode = WIFI_AUTH_OPEN;
	} else {
		ap_config.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
	}
	ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
	ESP_ERROR_CHECK(esp_wifi_start());

	esp_netif_ip_info_t ip;
	if (s_netif_ap != NULL && esp_netif_get_ip_info(s_netif_ap, &ip) == ESP_OK) {
		ip4addr_ntoa_r((const ip4_addr_t *)&ip.ip, s_ap_ip, sizeof(s_ap_ip));
	} else {
		snprintf(s_ap_ip, sizeof(s_ap_ip), "0.0.0.0");
	}

	s_wifi_running = true;
	ESP_LOGI(TAG_WIFI, "AP started, IP=%s", s_ap_ip);
	return ESP_OK;
}

static void wifi_ap_stop(void)
{
	if (!s_wifi_running) {
		return;
	}
	http_server_stop();
	ESP_ERROR_CHECK(esp_wifi_stop());
	ESP_ERROR_CHECK(esp_wifi_deinit());
	if (s_netif_ap != NULL) {
		esp_netif_destroy(s_netif_ap);
		s_netif_ap = NULL;
	}
	s_wifi_running = false;
	ESP_LOGI(TAG_WIFI, "AP stopped");
}

static void ui_task(void *arg)
{
	(void)arg;
	bool nav_prev = false;
	bool sel_prev = false;

	s_ui_state = UI_STATE_MENU_ACQ;

	while (true) {
		bool nav = button_pressed(NAV_BUTTON_GPIO);
		bool sel = button_pressed(SELECT_BUTTON_GPIO);

		if (nav && !nav_prev) {
			if (s_ui_state == UI_STATE_MENU_ACQ) {
				s_ui_state = UI_STATE_MENU_WEB;
			} else if (s_ui_state == UI_STATE_MENU_WEB) {
				s_ui_state = UI_STATE_MENU_ACQ;
			}
		}

		if (sel && !sel_prev) {
			if (s_ui_state == UI_STATE_MENU_ACQ) {
				if (start_acquisition()) {
					s_ui_state = UI_STATE_ACQ_RUNNING;
				}
			} else if (s_ui_state == UI_STATE_MENU_WEB) {
				if (wifi_ap_start() == ESP_OK && http_server_start() == ESP_OK) {
					s_ui_state = UI_STATE_WEB_RUNNING;
				} else {
					wifi_ap_stop();
					ESP_LOGW(TAG_UI, "Failed to start web mode");
				}
			} else if (s_ui_state == UI_STATE_ACQ_RUNNING) {
				stop_acquisition();
				s_ui_state = UI_STATE_SAVE_NOTICE;
			} else if (s_ui_state == UI_STATE_WEB_RUNNING) {
				wifi_ap_stop();
				s_ui_state = UI_STATE_MENU_WEB;
			}
		}

		nav_prev = nav;
		sel_prev = sel;

		uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000);
		if (s_ui_state == UI_STATE_SAVE_NOTICE && now_ms >= s_save_notice_until_ms) {
			s_ui_state = UI_STATE_MENU_ACQ;
		}

		u8g2_ClearBuffer(&s_u8g2);
		switch (s_ui_state) {
		case UI_STATE_MENU_ACQ:
			ui_draw_centered("Acquisition", "mode");
			break;
		case UI_STATE_MENU_WEB:
			ui_draw_centered("Web server", "mode");
			break;
		case UI_STATE_ACQ_RUNNING: {
			uint64_t elapsed_s = (now_ms - s_acq_start_ms) / 1000;
			unsigned int minutes = (unsigned int)((elapsed_s / 60) % 100);
			unsigned int seconds = (unsigned int)(elapsed_s % 60);
			char timer[12];
			snprintf(timer, sizeof(timer), "%02u:%02u", minutes, seconds);
			u8g2_DrawStr(&s_u8g2, 0, 10, timer);
			ui_draw_heartline(s_heart_phase++);
			break;
		}
		case UI_STATE_WEB_RUNNING:
			u8g2_DrawStr(&s_u8g2, 0, 10, "IP addr:");
			u8g2_DrawStr(&s_u8g2, 0, 24, s_ap_ip[0] ? s_ap_ip : "starting");
			break;
		case UI_STATE_SAVE_NOTICE:
			ui_draw_centered("Saved as", s_saved_name);
			break;
		default:
			break;
		}
		u8g2_SendBuffer(&s_u8g2);
		vTaskDelay(pdMS_TO_TICKS(UI_REFRESH_MS));
	}
}

static void acq_task(void *arg)
{
	(void)arg;
	while (true) {
		if (!s_acq_running) {
			vTaskDelay(pdMS_TO_TICKS(50));
			continue;
		}

		uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000);
		float eeg = NAN;
		float ecg = NAN;
		float gsr = NAN;

		(void)ads1115_read_channel_avg(0, &eeg);
		(void)ads1115_read_channel_avg(1, &ecg);
		(void)ads1115_read_channel_avg(2, &gsr);

		float temp_c = NAN;
		if (xSemaphoreTake(s_temp_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
			if (s_temp_state.valid) {
				temp_c = s_temp_state.temp_c;
			}
			xSemaphoreGive(s_temp_mutex);
		}

		if (xSemaphoreTake(s_acq_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
			if (s_log_file != NULL) {
				(void)data_logger_write_row(s_log_file, now_ms, eeg, ecg, gsr, temp_c);
			}
			xSemaphoreGive(s_acq_mutex);
		}

		vTaskDelay(pdMS_TO_TICKS(ACQ_SAMPLE_PERIOD_MS));
	}
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
			ESP_LOGW(TAG_TEMP, "DS18B20 not responding");
			vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(DS18B20_CONV_TIME_MS));
			continue;
		}

		vTaskDelay(pdMS_TO_TICKS(DS18B20_CONV_TIME_MS));
		float temp_c = 0.0f;
		ok = ds18b20_read_temperature(&temp_c);
		if (xSemaphoreTake(s_temp_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
			s_temp_state.valid = ok;
			if (ok) {
				s_temp_state.temp_c = temp_c;
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

	s_acq_mutex = xSemaphoreCreateMutex();
	if (s_acq_mutex == NULL) {
		ESP_LOGE(TAG_UI, "Failed to create acquisition mutex");
		return;
	}

	esp_err_t err = nvs_flash_init();
	if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		nvs_flash_erase();
		ESP_ERROR_CHECK(nvs_flash_init());
	}
	ESP_ERROR_CHECK(esp_netif_init());
	err = esp_event_loop_create_default();
	if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
		ESP_ERROR_CHECK(err);
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
	ESP_LOGI(TAG_TEMP, "DS18B20 bus idle level=%d", gpio_get_level(DS18B20_GPIO));
	ESP_LOGI(TAG_TEMP, "DS18B20 presence=%s", ds18b20_reset_pulse() ? "yes" : "no");

	gpio_config_t button_gpio = {
		.pin_bit_mask = (1ULL << NAV_BUTTON_GPIO) | (1ULL << SELECT_BUTTON_GPIO),
		.mode = GPIO_MODE_INPUT,
		.pull_up_en = GPIO_PULLUP_ENABLE,
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
		.intr_type = GPIO_INTR_DISABLE,
	};
	ESP_ERROR_CHECK(gpio_config(&button_gpio));

	i2c_master_bus_config_t bus_config = {
		.clk_source = I2C_CLK_SRC_DEFAULT,
		.i2c_port = I2C_PORT,
		.scl_io_num = I2C_SCL_GPIO,
		.sda_io_num = I2C_SDA_GPIO,
		.glitch_ignore_cnt = 7,
		.flags.enable_internal_pullup = true,
	};
	ESP_LOGI(TAG_I2C, "I2C init SDA=%d SCL=%d Freq=%d", I2C_SDA_GPIO, I2C_SCL_GPIO, I2C_FREQ_HZ);
	ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &s_i2c_bus));
	s_oled.bus = s_i2c_bus;

	i2c_device_config_t oled_config = {
		.dev_addr_length = I2C_ADDR_BIT_LEN_7,
		.device_address = OLED_I2C_ADDR,
		.scl_speed_hz = I2C_FREQ_HZ,
	};
	ESP_ERROR_CHECK(i2c_master_bus_add_device(s_oled.bus, &oled_config, &s_oled.dev));

	i2c_device_config_t ads_config = {
		.dev_addr_length = I2C_ADDR_BIT_LEN_7,
		.device_address = ADS1115_I2C_ADDR,
		.scl_speed_hz = I2C_FREQ_HZ,
	};
	ESP_ERROR_CHECK(i2c_master_bus_add_device(s_i2c_bus, &ads_config, &s_ads.dev));

	if (ads1115_init(s_ads.dev) != ESP_OK) {
		ESP_LOGW(TAG_I2C, "ADS1115 init failed or returned error");
	}
	if (data_logger_init() != ESP_OK) {
		ESP_LOGW(TAG_UI, "Data logger (SPIFFS) init failed");
	}

	u8g2_Setup_ssd1306_i2c_64x32_noname_f(
		&s_u8g2, U8G2_R0, u8x8_byte_esp32_i2c, u8x8_gpio_and_delay_esp32);
	u8x8_SetI2CAddress(&s_u8g2.u8x8, OLED_I2C_ADDR << 1);
	u8g2_InitDisplay(&s_u8g2);
	u8g2_SetPowerSave(&s_u8g2, 0);
	u8g2_SetFont(&s_u8g2, u8g2_font_5x7_tr);
	ui_show_boot_splash();

	xTaskCreate(ui_task, "ui_task", 4096, NULL, 5, NULL);
	xTaskCreate(acq_task, "acq_task", 4096, NULL, 5, NULL);
	xTaskCreate(temp_task, "temp_task", 4096, NULL, 5, NULL);
}