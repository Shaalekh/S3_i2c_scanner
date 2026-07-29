# S3_i2c_scanner

## Project Overview
This project is an ESP-IDF firmware application for ESP32-S3 that combines:
- I2C bus setup and shared bus access control
- SSD1306-compatible 64x32 OLED display output
- ADS1115 device registration on the I2C bus
- DS18B20 1-Wire temperature acquisition
- FreeRTOS task-based runtime for sensor sampling and display refresh

The runtime continuously reads temperature from DS18B20, converts it to Celsius/Fahrenheit, and renders both values on the OLED.

## Hardware Components
- **ESP32-S3 MCU**
  - Runs FreeRTOS and all peripheral drivers.
- **SSD1306 OLED (I2C, address `0x3C`)**
  - 64x32 pixel monochrome panel.
  - Used to display live temperature values.
- **ADS1115 ADC (I2C, address `0x48`)**
  - Added to the I2C bus during initialization.
  - Device handle is created and available for future ADC expansion.
- **DS18B20 Temperature Sensor (1-Wire on GPIO10)**
  - Used as the active temperature source.
  - Uses reset/presence detect, conversion trigger, scratchpad read, and CRC validation.

## Pin and Bus Configuration
- **I2C Port:** `I2C_NUM_0`
- **SDA:** GPIO `8`
- **SCL:** GPIO `9`
- **I2C Frequency:** `400000 Hz`
- **OLED Address:** `0x3C`
- **ADS1115 Address:** `0x48`
- **DS18B20 Data Pin:** GPIO `10` (open-drain with pull-up)

## Software Components
### 1. Synchronization and Shared State
- **`s_i2c_mutex`**
  - Protects all I2C transactions so multiple tasks do not collide on the bus.
- **`s_temp_mutex`**
  - Protects shared temperature state (`s_temp_state`) accessed by producer/consumer tasks.
- **`s_temp_state`**
  - Holds latest `temp_c`, `temp_f`, and validity flag.

### 2. OLED Rendering Pipeline
- **Framebuffer:** `s_framebuffer[OLED_WIDTH * OLED_PAGES]`
- **Primitive drawing:** `set_pixel()`
- **Font system:** `glyph_t`, `s_font[]`, `find_glyph()`
- **Text renderers:** `draw_char()`, `draw_text()`
- **Display transport:**
  - `oled_write_cmd()` sends command bytes
  - `oled_write_data()` sends page payloads
  - `oled_show()` pushes all pages to display
- **Initialization sequence:** `oled_init_display()` configures SSD1306 registers

### 3. DS18B20 Driver Flow
- **Bus control:** `ds18b20_drive_low()`, `ds18b20_release_bus()`, `ds18b20_read_bus()`
- **1-Wire protocol:** `ds18b20_reset_pulse()`, bit/byte read-write helpers
- **Integrity check:** `ds18b20_crc8()`
- **Sampling operations:**
  - `ds18b20_start_conversion()`
  - wait conversion time (`750 ms`)
  - `ds18b20_read_scratchpad()`
  - `ds18b20_read_temperature()`

### 4. FreeRTOS Tasks
- **`temp_task`**
  - Triggers DS18B20 conversion
  - Waits conversion time
  - Reads and validates temperature
  - Updates shared temperature state
- **`oled_task`**
  - Reads latest temperature snapshot
  - Formats strings (`C:` and `F:`)
  - Renders to framebuffer
  - Writes framebuffer to OLED every 250 ms

## Detailed Runtime Workflow
1. **Startup (`app_main`)**
   - Create I2C and temperature mutexes.
   - Configure DS18B20 GPIO as open-drain + pull-up.
   - Initialize I2C master bus on configured pins/speed.
   - Add OLED and ADS1115 as I2C devices.
   - Initialize OLED with command sequence under I2C lock.
   - Spawn `oled_task` and `temp_task`.

2. **Temperature Acquisition Cycle (`temp_task`)**
   - Send DS18B20 reset pulse and `SKIP_ROM + CONVERT_T`.
   - Delay for conversion completion (`750 ms`).
   - Read scratchpad and verify CRC.
   - Convert raw sensor data to Celsius and Fahrenheit.
   - Publish valid/invalid reading into shared state.

3. **Display Update Cycle (`oled_task`)**
   - Read shared temperature state safely.
   - Build display text (or placeholder when invalid).
   - Draw text into framebuffer.
   - Acquire I2C lock and transfer display pages.
   - Repeat every 250 ms.

4. **Concurrency Model**
   - I2C operations are serialized by `s_i2c_mutex`.
   - Temperature state is serialized by `s_temp_mutex`.
   - Display and sensor tasks run independently but share data safely.

## Build and Flash Workflow (ESP-IDF)
From repository root:
1. Export ESP-IDF environment.
2. Configure target if needed: `idf.py set-target esp32s3`
3. Build: `idf.py build`
4. Flash: `idf.py -p <PORT> flash`
5. Monitor logs: `idf.py monitor`

## Current Functional Scope
- Temperature readout from DS18B20 is fully active.
- OLED output of Celsius/Fahrenheit is active.
- ADS1115 is currently initialized on I2C bus for future ADC feature extension.
