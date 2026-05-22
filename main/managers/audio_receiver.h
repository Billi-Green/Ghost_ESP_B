#ifndef AUDIO_RECEIVER_H
#define AUDIO_RECEIVER_H

#include <stdint.h>
#include <stdbool.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

// Audio receiver configuration
typedef struct {
    int uart_tx_pin;       // UART TX pin (to C5 RX)
    int uart_rx_pin;       // UART RX pin (from C5 TX)
    int uart_baud;         // UART baud rate
    int i2s_bclk_pin;      // I2S BCLK pin
    int i2s_lrclk_pin;     // I2S LRCLK/WCLK pin
    int i2s_dout_pin;      // I2S DOUT pin
    int i2s_mclk_pin;      // I2S MCLK pin (-1 = unused)
    int dac_i2c_sda_pin;   // DAC I2C SDA pin
    int dac_i2c_scl_pin;   // DAC I2C SCL pin
    int dac_i2c_addr;      // DAC I2C address (0x18)
    int dac_i2c_port;      // I2C port number
    float i2s_gain;        // I2S output gain (0.0 - 1.0)
} audio_receiver_config_t;

// Default configuration for ESP32-S3
#define AUDIO_RECEIVER_S3_DEFAULT_CONFIG() { \
    .uart_tx_pin = 9, \
    .uart_rx_pin = 10, \
    .uart_baud = 460800, \
    .i2s_bclk_pin = 46, \
    .i2s_lrclk_pin = 45, \
    .i2s_dout_pin = 1, \
    .i2s_mclk_pin = -1, \
    .dac_i2c_sda_pin = -1, \
    .dac_i2c_scl_pin = -1, \
    .dac_i2c_addr = 0x18, \
    .dac_i2c_port = 0, \
    .i2s_gain = 0.5f \
}

// Audio receiver state
typedef enum {
    AUDIO_RX_STATE_IDLE = 0,
    AUDIO_RX_STATE_RECEIVING,
    AUDIO_RX_STATE_PLAYING,
    AUDIO_RX_STATE_ERROR
} audio_rx_state_t;

// Function prototypes
esp_err_t audio_receiver_init(const audio_receiver_config_t *config);
esp_err_t audio_receiver_deinit(void);
bool audio_receiver_is_initialized(void);

// DAC control (via I2C if pins configured)
esp_err_t audio_rx_dac_init(void);
esp_err_t audio_rx_dac_set_volume(uint8_t volume);

// I2S control
esp_err_t audio_rx_i2s_init(void);
esp_err_t audio_rx_i2s_set_gain(float gain);

// State
audio_rx_state_t audio_rx_get_state(void);

#ifdef __cplusplus
}
#endif

#endif // AUDIO_RECEIVER_H
