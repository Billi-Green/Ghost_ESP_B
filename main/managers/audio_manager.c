#include "managers/audio_manager.h"
#include "managers/sd_card_manager.h"
#include "io_manager.h"
#include "core/esp_comm_manager.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

// IO expander address from config
#ifndef CONFIG_IO_EXPANDER_I2C_ADDR
#define CONFIG_IO_EXPANDER_I2C_ADDR 0x74
#endif
#define IO_EXPANDER_I2C_ADDR CONFIG_IO_EXPANDER_I2C_ADDR

static const char *TAG = "AUDIO_MANAGER";

#define AUDIO_STREAM_CHUNK_SIZE 512
#define AUDIO_STREAM_BATCH_CHUNKS 8

// DAC TLV320DAC3100 register addresses (Page 0 unless noted)
#define DAC_REG_PAGE_CTRL      0x00
#define DAC_REG_SW_RESET       0x01
#define DAC_REG_CLK_MUX        0x04
#define DAC_REG_CLK_PLL_PR     0x05
#define DAC_REG_CLK_PLL_J      0x06
#define DAC_REG_CLK_PLL_D_MSB  0x07
#define DAC_REG_CLK_PLL_D_LSB  0x08
#define DAC_REG_NDAC           0x0B
#define DAC_REG_MDAC           0x0C
#define DAC_REG_DOSR_MSB       0x0D
#define DAC_REG_DOSR_LSB       0x0E
#define DAC_REG_I2S_SETUP      0x1B
#define DAC_REG_DAC_PRB        0x3C
#define DAC_REG_DAC_DATAPATH   0x3F
#define DAC_REG_DAC_VOL_CTRL   0x40
#define DAC_REG_DAC_CH1_VOL    0x41
#define DAC_REG_DAC_CH2_VOL    0x42

// Page 1 analog registers
#define DAC_REG_OUT_ROUTING    0x23
#define DAC_REG_HPL_VOL        0x24
#define DAC_REG_HPR_VOL        0x25
#define DAC_REG_SPK_VOL        0x26
#define DAC_REG_HPL_DRIVER     0x28
#define DAC_REG_HPR_DRIVER     0x29
#define DAC_REG_SPK_DRIVER     0x2A
#define DAC_REG_HP_DRIVER_CTRL 0x2C
#define DAC_REG_AIN_CM_CTRL    0x32
#define DAC_REG_HP_DRIVERS     0x1F
#define DAC_REG_SPK_AMP        0x20

// Audio state
static audio_manager_config_t g_config;
static bool g_initialized = false;
static audio_state_t g_state = AUDIO_STATE_IDLE;
static char g_current_file[512] = {0};
static FILE *g_mp3_file = NULL;
static mp3_file_info_t *g_file_list = NULL;
static int g_file_count = 0;
static TaskHandle_t g_stream_task_handle = NULL;
static StaticTask_t *g_stream_task_buf = NULL;
static StackType_t *g_stream_stack_buf = NULL;
static bool g_stream_running = false;
static bool g_sd_jit_mounted = false;
static bool g_sd_display_suspended = false;

// Forward declarations
static void audio_stream_task(void *arg);
static esp_err_t dac_write_reg(uint8_t reg, uint8_t val);
static esp_err_t dac_read_reg(uint8_t reg, uint8_t *val);
static esp_err_t dac_set_page(uint8_t page);

esp_err_t audio_manager_init(const audio_manager_config_t *config)
{
    if (g_initialized) {
        ESP_LOGW(TAG, "Audio manager already initialized");
        return ESP_OK;
    }

    if (!config) {
        ESP_LOGE(TAG, "Invalid configuration");
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(&g_config, config, sizeof(audio_manager_config_t));

    ESP_LOGI(TAG, "Initializing audio manager");
    ESP_LOGI(TAG, "SD CS=%d, CLK=%d, MISO=%d, MOSI=%d", g_config.sd_cs_pin, g_config.sd_clk_pin, g_config.sd_miso_pin, g_config.sd_mosi_pin);
    ESP_LOGI(TAG, "DAC I2C addr=0x%02X, Reset P%d.%d", g_config.dac_i2c_addr, 
             g_config.dac_reset_io_expander_port, g_config.dac_reset_io_expander_bit);

    // Allocate file list from PSRAM to conserve internal RAM (~99KB)
    g_file_list = heap_caps_malloc(128 * sizeof(mp3_file_info_t), MALLOC_CAP_SPIRAM);
    if (!g_file_list) {
        ESP_LOGE(TAG, "Failed to allocate file list from PSRAM");
        return ESP_ERR_NO_MEM;
    }
    memset(g_file_list, 0, 128 * sizeof(mp3_file_info_t));

    // Step 1: Reset DAC via IO expander
    ESP_LOGI(TAG, "Resetting DAC via IO expander...");
    esp_err_t ret = audio_dac_reset();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "DAC reset failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Step 2: Initialize DAC via I2C
    ESP_LOGI(TAG, "Initializing DAC via I2C...");
    ret = audio_dac_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "DAC init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    g_initialized = true;
    g_state = AUDIO_STATE_IDLE;
    ESP_LOGI(TAG, "Audio manager initialized successfully");

    return ESP_OK;
}

esp_err_t audio_manager_deinit(void)
{
    if (!g_initialized) {
        return ESP_OK;
    }

    audio_stop();

    if (g_file_list) {
        heap_caps_free(g_file_list);
        g_file_list = NULL;
    }

    g_initialized = false;
    g_state = AUDIO_STATE_IDLE;

    ESP_LOGI(TAG, "Audio manager deinitialized");
    return ESP_OK;
}

bool audio_manager_is_initialized(void)
{
    return g_initialized;
}

// DAC Reset via IO Expander
esp_err_t audio_dac_reset(void)
{
    if (!io_manager_is_initialized()) {
        ESP_LOGE(TAG, "IO manager not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    // Configure the reset pin as output on the IO expander
    // TCA9539: Port 1 config register is 0x07, output register is 0x03
    uint8_t config_port = (g_config.dac_reset_io_expander_port == 0) ? 0x06 : 0x07;
    uint8_t output_port = (g_config.dac_reset_io_expander_port == 0) ? 0x02 : 0x03;
    uint8_t bit = g_config.dac_reset_io_expander_bit;

    // Read current config
    uint8_t current_config;
    esp_err_t ret = i2c_read_reg8_direct(IO_EXPANDER_I2C_ADDR, config_port, &current_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read IO expander config: %s", esp_err_to_name(ret));
        return ret;
    }

    // Set the bit as output (0 = output, 1 = input)
    current_config &= ~(1 << bit);
    ret = i2c_write_reg8_direct(IO_EXPANDER_I2C_ADDR, config_port, current_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write IO expander config: %s", esp_err_to_name(ret));
        return ret;
    }

    // Read current output
    uint8_t current_output;
    ret = i2c_read_reg8_direct(IO_EXPANDER_I2C_ADDR, output_port, &current_output);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read IO expander output: %s", esp_err_to_name(ret));
        return ret;
    }

    // Pull reset low (0 = low, 1 = high)
    current_output &= ~(1 << bit);
    ret = i2c_write_reg8_direct(IO_EXPANDER_I2C_ADDR, output_port, current_output);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write IO expander output (low): %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "DAC reset pulled LOW");
    vTaskDelay(pdMS_TO_TICKS(g_config.dac_reset_duration_ms));

    // Release reset (set high)
    current_output |= (1 << bit);
    ret = i2c_write_reg8_direct(IO_EXPANDER_I2C_ADDR, output_port, current_output);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write IO expander output (high): %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "DAC reset released (HIGH)");
    vTaskDelay(pdMS_TO_TICKS(10));  // Wait for DAC to stabilize

    return ESP_OK;
}

// DAC I2C write helper
static esp_err_t dac_write_reg(uint8_t reg, uint8_t val)
{
    return i2c_write_reg8_direct(g_config.dac_i2c_addr, reg, val);
}

// DAC I2C read helper
static esp_err_t dac_read_reg(uint8_t reg, uint8_t *val)
{
    return i2c_read_reg8_direct(g_config.dac_i2c_addr, reg, val);
}

// DAC set page
static esp_err_t dac_set_page(uint8_t page)
{
    return dac_write_reg(DAC_REG_PAGE_CTRL, page);
}

// DAC initialization sequence
esp_err_t audio_dac_init(void)
{
    esp_err_t ret;

    // Software reset
    ret = dac_set_page(0);
    if (ret != ESP_OK) return ret;
    ret = dac_write_reg(DAC_REG_SW_RESET, 0x01);
    if (ret != ESP_OK) return ret;
    vTaskDelay(pdMS_TO_TICKS(10));

    // Clock settings - use internal PLL with BCLK as reference (matching Arduino)
    // 0x06 = CODEC_CLKIN from PLL (01), PLL_CLKIN from BCLK (10)
    ret = dac_write_reg(DAC_REG_CLK_MUX, 0x06);
    if (ret != ESP_OK) return ret;

    // PLL: P=1, R=1, J=8, D=0 (matching Arduino exactly)
    // BCLK = 44100 × 16b slot × 2ch = 1.4112 MHz
    // PLLCLK = 1.4112 MHz × 8 = 11.29 MHz (Arduino uses this and works)
    ret = dac_write_reg(DAC_REG_CLK_PLL_PR, 0x91);  // PLL on, P=1, R=1
    if (ret != ESP_OK) return ret;
    ret = dac_write_reg(DAC_REG_CLK_PLL_J, 0x08);  // J=8
    if (ret != ESP_OK) return ret;
    ret = dac_write_reg(DAC_REG_CLK_PLL_D_MSB, 0x00);  // D=0
    if (ret != ESP_OK) return ret;
    ret = dac_write_reg(DAC_REG_CLK_PLL_D_LSB, 0x00);
    if (ret != ESP_OK) return ret;

    // NDAC=2, MDAC=2 (matching Arduino)
    ret = dac_write_reg(DAC_REG_NDAC, 0x82);  // NDAC=2, powered
    if (ret != ESP_OK) return ret;
    ret = dac_write_reg(DAC_REG_MDAC, 0x82);  // MDAC=2, powered
    if (ret != ESP_OK) return ret;

    // DOSR = 128 (matching Arduino)
    ret = dac_write_reg(DAC_REG_DOSR_MSB, 0x00);
    if (ret != ESP_OK) return ret;
    ret = dac_write_reg(DAC_REG_DOSR_LSB, 0x80);  // DOSR=128
    if (ret != ESP_OK) return ret;

    // Codec interface: I2S (Philips) mode, 16-bit (matching Arduino)
    ret = dac_write_reg(DAC_REG_I2S_SETUP, 0x00);  // D7:D6=00(I2S), D5:D4=00(16b), slave
    if (ret != ESP_OK) return ret;

    // DAC processing block - PRB_P1
    ret = dac_write_reg(DAC_REG_DAC_PRB, 0x01);
    if (ret != ESP_OK) return ret;

    // --- Page 1: Analog output setup BEFORE powering digital DAC path ---
    ret = dac_set_page(1);
    if (ret != ESP_OK) return ret;

    // LDO / Power setup (required for analog output stage)
    ret = dac_write_reg(0x02, 0x09);  // AVDD LDO power up (initial)
    if (ret != ESP_OK) return ret;
    ret = dac_write_reg(0x01, 0x08);  // Disable weak AVDD (use external supply)
    if (ret != ESP_OK) return ret;
    ret = dac_write_reg(0x02, 0x01);  // AVDD LDO powered, 1.72V
    if (ret != ESP_OK) return ret;

    // Common mode control: CM=0.9V, HP CM=1.65V, Line CM=1.65V, LDOin supply
    ret = dac_write_reg(0x0A, 0x3B);
    if (ret != ESP_OK) return ret;

    // DAC PTM (Power Tuning Mode) = PTM_P1 for both channels
    ret = dac_write_reg(0x03, 0x08);  // DAC Left PTM_P1
    if (ret != ESP_OK) return ret;
    ret = dac_write_reg(0x04, 0x08);  // DAC Right PTM_P1
    if (ret != ESP_OK) return ret;

    // Depop control: 6kohm, N=6.0
    ret = dac_write_reg(0x14, 0x29);
    if (ret != ESP_OK) return ret;

    // Route DAC outputs to headphone drivers
    ret = dac_write_reg(0x0C, 0x08);  // HPL routed from DAC_L
    if (ret != ESP_OK) return ret;
    ret = dac_write_reg(0x0D, 0x08);  // HPR routed from DAC_R
    if (ret != ESP_OK) return ret;

    // HP driver gains: 0dB (unmuted)
    ret = dac_write_reg(0x10, 0x00);  // HPL gain 0dB
    if (ret != ESP_OK) return ret;
    ret = dac_write_reg(0x11, 0x00);  // HPR gain 0dB
    if (ret != ESP_OK) return ret;

    // Power up HP drivers (both channels)
    ret = dac_write_reg(0x09, 0x30);  // HPL + HPR powered
    if (ret != ESP_OK) return ret;

    // Wait for analog gain to fully apply
    vTaskDelay(pdMS_TO_TICKS(100));

    // Power up Class-D speaker amplifier
    ret = dac_write_reg(0x20, 0x80);  // SPK amp powered (D7=1)
    if (ret != ESP_OK) return ret;

    // Route DAC to speaker output
    ret = dac_write_reg(0x23, 0x08);  // SPK routed from DAC
    if (ret != ESP_OK) return ret;

    // SPK analog volume: enabled, 0dB
    ret = dac_write_reg(0x26, 0x80);
    if (ret != ESP_OK) return ret;

    // Speaker PGA gain: 6dB, unmuted
    ret = dac_write_reg(0x2A, 0x04);  // D5:D4=00(6dB), D2=1(unmuted)
    if (ret != ESP_OK) return ret;

    // Wait for analog circuits to stabilize
    vTaskDelay(pdMS_TO_TICKS(50));

    // --- Back to Page 0: Power up DAC datapath (AFTER analog path is ready) ---
    ret = dac_set_page(0);
    if (ret != ESP_OK) return ret;

    // DAC datapath: both channels powered, normal routing, soft-step enabled
    ret = dac_write_reg(DAC_REG_DAC_DATAPATH, 0xD6);
    if (ret != ESP_OK) return ret;

    // Unmute DAC, independent volume control mode
    ret = dac_write_reg(DAC_REG_DAC_VOL_CTRL, 0x00);
    if (ret != ESP_OK) return ret;

    // Digital volume: 0dB full scale on both channels (matching Arduino)
    ret = dac_write_reg(DAC_REG_DAC_CH1_VOL, 0x00);
    if (ret != ESP_OK) return ret;
    ret = dac_write_reg(DAC_REG_DAC_CH2_VOL, 0x00);
    if (ret != ESP_OK) return ret;

    ESP_LOGI(TAG, "DAC TLV320DAC3100 initialized successfully");
    return ESP_OK;
}

esp_err_t audio_dac_set_volume(uint8_t volume)
{
    // Volume: 0 = mute, 1-255 = -63.5dB to 0dB (0.5dB steps)
    // DAC uses signed magnitude: bit 7 = mute, bits 6-0 = magnitude
    uint8_t dac_vol = volume >> 1;  // Convert 0-255 to 0-127
    if (volume == 0) {
        dac_vol = 0x80;  // Mute
    }

    esp_err_t ret = dac_write_reg(DAC_REG_DAC_CH1_VOL, dac_vol);
    if (ret != ESP_OK) return ret;
    ret = dac_write_reg(DAC_REG_DAC_CH2_VOL, dac_vol);
    return ret;
}

esp_err_t audio_stream_send(const uint8_t *data, size_t length)
{
    if (!g_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!esp_comm_manager_is_connected()) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!esp_comm_manager_send_stream(COMM_STREAM_CHANNEL_AUDIO, data, length)) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

// Scan for MP3 files
int audio_scan_mp3_files(mp3_file_info_t *files, int max_files, const char *directory)
{
    if (!files || max_files <= 0 || !directory) {
        return 0;
    }

    bool display_suspended = false;
    esp_err_t err = sd_card_mount_for_flush(&display_suspended);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD mount failed for scan: %s", esp_err_to_name(err));
        return 0;
    }

    DIR *dir = opendir(directory);
    if (!dir) {
        ESP_LOGE(TAG, "Failed to open directory: %s", directory);
        sd_card_unmount_after_flush(display_suspended);
        return 0;
    }

    int count = 0;
    struct dirent *entry;

    while ((entry = readdir(dir)) != NULL && count < max_files) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        // Check for .mp3 extension
        size_t len = strlen(entry->d_name);
        if (len > 4 && strcasecmp(entry->d_name + len - 4, ".mp3") == 0) {
            strncpy(files[count].filename, entry->d_name, sizeof(files[count].filename) - 1);
            files[count].filename[sizeof(files[count].filename) - 1] = '\0';

            snprintf(files[count].full_path, sizeof(files[count].full_path), "%s/%s", directory, entry->d_name);

            // Get file size
            struct stat st;
            if (stat(files[count].full_path, &st) == 0) {
                files[count].size = st.st_size;
            } else {
                files[count].size = 0;
            }

            ESP_LOGI(TAG, "Found MP3: %s (%lu bytes)", files[count].filename, (unsigned long)files[count].size);
            count++;
        }
    }

    closedir(dir);
    sd_card_unmount_after_flush(display_suspended);
    g_file_count = count;
    ESP_LOGI(TAG, "Found %d MP3 files in %s", count, directory);
    return count;
}

// Streaming task
static void audio_stream_task(void *arg)
{
    (void)arg;
    uint8_t *batch_buf = heap_caps_malloc(AUDIO_STREAM_CHUNK_SIZE * AUDIO_STREAM_BATCH_CHUNKS, MALLOC_CAP_SPIRAM);
    if (!batch_buf) {
        ESP_LOGE(TAG, "Failed to allocate batch buffer");
        g_state = AUDIO_STATE_ERROR;
        g_stream_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    long file_pos = 0;
    char filepath[512];
    strncpy(filepath, g_current_file, sizeof(filepath) - 1);
    filepath[sizeof(filepath) - 1] = '\0';
    int consecutive_failures = 0;
    int chunk_count = 0;
    FILE *stream_file = NULL;

    ESP_LOGI(TAG, "Stream task started for: %s (batch=%d chunks)", filepath, AUDIO_STREAM_BATCH_CHUNKS);

    // Mount SD once at the start of streaming
    bool display_suspended = false;
    esp_err_t err = sd_card_mount_for_flush(&display_suspended);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD mount failed in stream task: %s", esp_err_to_name(err));
        g_state = AUDIO_STATE_ERROR;
        heap_caps_free(batch_buf);
        g_stream_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }
    g_sd_jit_mounted = true;
    g_sd_display_suspended = display_suspended;

    // Open file once and keep it open during streaming
    stream_file = fopen(filepath, "rb");
    if (!stream_file) {
        ESP_LOGE(TAG, "Failed to open file in stream task");
        sd_card_unmount_after_flush(g_sd_display_suspended);
        g_sd_jit_mounted = false;
        g_sd_display_suspended = false;
        g_state = AUDIO_STATE_ERROR;
        heap_caps_free(batch_buf);
        g_stream_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    while (g_stream_running) {
        if (g_state != AUDIO_STATE_PLAYING) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (!esp_comm_manager_is_connected()) {
            ESP_LOGW(TAG, "Peer not connected, waiting...");
            vTaskDelay(pdMS_TO_TICKS(500));
            consecutive_failures = 0;
            continue;
        }

        // Read a batch of chunks from the already-open file
        size_t batch_size = AUDIO_STREAM_CHUNK_SIZE * AUDIO_STREAM_BATCH_CHUNKS;
        size_t total_read = 0;
        while (total_read < batch_size) {
            size_t n = fread(batch_buf + total_read, 1, batch_size - total_read, stream_file);
            if (n == 0) break;
            total_read += n;
        }
        file_pos += total_read;

        // Send batched data
        if (total_read > 0) {
            size_t sent = 0;
            bool batch_ok = true;
            while (sent < total_read) {
                size_t chunk = total_read - sent;
                if (chunk > AUDIO_STREAM_CHUNK_SIZE) chunk = AUDIO_STREAM_CHUNK_SIZE;
                esp_err_t ret = audio_stream_send(batch_buf + sent, chunk);
                if (ret == ESP_OK) {
                    consecutive_failures = 0;
                    chunk_count++;
                    // Throttle to match audio bitrate (~40 kB/s for 320 kbps MP3).
                    // At 921600 baud, a 512-byte chunk (~576 bytes on wire) takes ~5ms.
                    // 6ms delay + 5ms transmit = 11ms cycle → ~46 KB/s → ~318 kbps after batch overhead.
                    vTaskDelay(pdMS_TO_TICKS(6));
                } else {
                    consecutive_failures++;
                    batch_ok = false;
                    ESP_LOGW(TAG, "Stream send failed: %s (consecutive: %d)", esp_err_to_name(ret), consecutive_failures);
                    if (consecutive_failures > 10) {
                        ESP_LOGE(TAG, "Too many consecutive send failures, stopping");
                        g_state = AUDIO_STATE_ERROR;
                        break;
                    }
                    vTaskDelay(pdMS_TO_TICKS(1));
                }
                sent += chunk;
            }
            if (chunk_count % 50 == 0) {
                ESP_LOGI(TAG, "Sent %d chunks (%ld bytes streamed)", chunk_count, file_pos);
            }
            if (!batch_ok && consecutive_failures > 10) break;
        }

        if (total_read < batch_size) {
            // End of file, loop
            ESP_LOGI(TAG, "End of file reached (%ld bytes), looping", file_pos);
            file_pos = 0;
            consecutive_failures = 0;
            fseek(stream_file, 0, SEEK_SET);
        }

        // Yield to LVGL for display flushes
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    ESP_LOGI(TAG, "Stream task exiting");
    heap_caps_free(batch_buf);

    // Close file and unmount SD
    if (stream_file) {
        fclose(stream_file);
    }
    if (g_sd_jit_mounted) {
        sd_card_unmount_after_flush(g_sd_display_suspended);
        g_sd_jit_mounted = false;
        g_sd_display_suspended = false;
    }

    g_stream_task_handle = NULL;
    vTaskDelete(NULL);
}

// Play MP3 file
esp_err_t audio_play_file(const char *filepath)
{
    if (!g_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    // Stop current playback
    audio_stop();

    strncpy(g_current_file, filepath, sizeof(g_current_file) - 1);
    g_current_file[sizeof(g_current_file) - 1] = '\0';

    ESP_LOGI(TAG, "Playing: %s", g_current_file);

    g_stream_running = true;
    g_stream_task_handle = NULL;
    g_stream_task_buf = heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL);
    g_stream_stack_buf = heap_caps_malloc(4096 * sizeof(StackType_t), MALLOC_CAP_SPIRAM);
    if (!g_stream_task_buf || !g_stream_stack_buf) {
        ESP_LOGE(TAG, "Failed to allocate task buffers");
        heap_caps_free(g_stream_task_buf);
        heap_caps_free(g_stream_stack_buf);
        g_stream_task_buf = NULL;
        g_stream_stack_buf = NULL;
        g_state = AUDIO_STATE_ERROR;
        return ESP_FAIL;
    }
    g_stream_task_handle = xTaskCreateStatic(audio_stream_task, "audio_stream", 4096, NULL, 4, g_stream_stack_buf, g_stream_task_buf);
    if (!g_stream_task_handle) {
        ESP_LOGE(TAG, "Failed to create stream task");
        heap_caps_free(g_stream_task_buf);
        heap_caps_free(g_stream_stack_buf);
        g_stream_task_buf = NULL;
        g_stream_stack_buf = NULL;
        g_state = AUDIO_STATE_ERROR;
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Stream task created, handle=%p", g_stream_task_handle);

    g_state = AUDIO_STATE_PLAYING;
    return ESP_OK;
}

esp_err_t audio_play_file_by_index(int index)
{
    if (!g_file_list) {
        ESP_LOGE(TAG, "File list not allocated");
        return ESP_ERR_INVALID_STATE;
    }
    if (index < 0 || index >= g_file_count) {
        ESP_LOGE(TAG, "Invalid file index: %d", index);
        return ESP_ERR_INVALID_ARG;
    }

    return audio_play_file(g_file_list[index].full_path);
}

esp_err_t audio_stop(void)
{
    g_stream_running = false;

    if (g_stream_task_handle) {
        vTaskDelay(pdMS_TO_TICKS(50));
        if (g_stream_task_handle) {
            vTaskDelete(g_stream_task_handle);
            g_stream_task_handle = NULL;
            heap_caps_free(g_stream_task_buf);
            heap_caps_free(g_stream_stack_buf);
            g_stream_task_buf = NULL;
            g_stream_stack_buf = NULL;
        }
    }

    if (g_sd_jit_mounted) {
        sd_card_unmount_after_flush(g_sd_display_suspended);
        g_sd_jit_mounted = false;
        g_sd_display_suspended = false;
    }

    g_mp3_file = NULL;
    g_current_file[0] = '\0';
    g_state = AUDIO_STATE_IDLE;

    ESP_LOGI(TAG, "Audio stopped");
    return ESP_OK;
}

esp_err_t audio_pause(void)
{
    if (g_state == AUDIO_STATE_PLAYING) {
        g_state = AUDIO_STATE_PAUSED;
        ESP_LOGI(TAG, "Audio paused");
    }
    return ESP_OK;
}

esp_err_t audio_resume(void)
{
    if (g_state == AUDIO_STATE_PAUSED) {
        g_state = AUDIO_STATE_PLAYING;
        ESP_LOGI(TAG, "Audio resumed");
    }
    return ESP_OK;
}

audio_state_t audio_get_state(void)
{
    return g_state;
}

const char* audio_get_current_file(void)
{
    return g_current_file;
}

int audio_get_file_count(void)
{
    return g_file_count;
}

const mp3_file_info_t* audio_get_file_list(void)
{
    return g_file_list;
}
