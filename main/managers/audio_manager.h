#ifndef AUDIO_MANAGER_H
#define AUDIO_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

// Audio manager configuration
typedef struct {
    int uart_tx_pin;       // UART TX pin (to S3 RX)
    int uart_rx_pin;       // UART RX pin (from S3 TX)
    int uart_baud;         // UART baud rate
    int sd_cs_pin;         // SD card CS pin
    int sd_clk_pin;        // SD card CLK pin
    int sd_miso_pin;       // SD card MISO pin
    int sd_mosi_pin;       // SD card MOSI pin
    int dac_i2c_addr;      // DAC I2C address (0x18)
    int dac_reset_io_expander_port;  // IO expander port for DAC reset (1 for P1x)
    int dac_reset_io_expander_bit;   // IO expander bit for DAC reset (3 for Px3)
    int dac_reset_duration_ms;       // DAC reset pulse duration in ms
} audio_manager_config_t;

// Default configuration for ESP32-C5
#define AUDIO_MANAGER_C5_DEFAULT_CONFIG() { \
    .uart_tx_pin = 13, \
    .uart_rx_pin = 14, \
    .uart_baud = 460800, \
    .sd_cs_pin = 0, \
    .sd_clk_pin = 4, \
    .sd_miso_pin = 2, \
    .sd_mosi_pin = 5, \
    .dac_i2c_addr = 0x18, \
    .dac_reset_io_expander_port = 1, \
    .dac_reset_io_expander_bit = 3, \
    .dac_reset_duration_ms = 50 \
}

// MP3 file info
typedef struct {
    char filename[256];
    char full_path[512];
    uint32_t size;
} mp3_file_info_t;

// Audio state
typedef enum {
    AUDIO_STATE_IDLE = 0,
    AUDIO_STATE_PLAYING,
    AUDIO_STATE_PAUSED,
    AUDIO_STATE_ERROR
} audio_state_t;

// Function prototypes
esp_err_t audio_manager_init(const audio_manager_config_t *config);
esp_err_t audio_manager_deinit(void);
bool audio_manager_is_initialized(void);

// DAC control
esp_err_t audio_dac_reset(void);
esp_err_t audio_dac_init(void);
esp_err_t audio_dac_set_volume(uint8_t volume);  // 0-255

// MP3 file management
int audio_scan_mp3_files(mp3_file_info_t *files, int max_files, const char *directory);
esp_err_t audio_play_file(const char *filepath);
esp_err_t audio_play_file_by_index(int index);
esp_err_t audio_stop(void);
esp_err_t audio_pause(void);
esp_err_t audio_resume(void);
audio_state_t audio_get_state(void);
const char* audio_get_current_file(void);
int audio_get_file_count(void);
const mp3_file_info_t* audio_get_file_list(void);

// GhostLink streaming
esp_err_t audio_stream_send(const uint8_t *data, size_t length);

#ifdef __cplusplus
}
#endif

#endif // AUDIO_MANAGER_H
