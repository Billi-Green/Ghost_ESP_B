#include "managers/audio_receiver.h"
#include "core/esp_comm_manager.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include <stdio.h>
#include <string.h>

#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"

#ifndef MINIMP3_MAX_FRAME_SIZE
#define MINIMP3_MAX_FRAME_SIZE 16384
#endif

static const char *TAG = "AUDIO_RECEIVER";

#define AUDIO_RX_I2S_BUF_COUNT 8
#define AUDIO_RX_I2S_PORT      I2S_NUM_1
#define AUDIO_RX_I2S_SAMPLE_RATE 44100
#define AUDIO_RX_I2S_BITS_PER_SAMPLE I2S_DATA_BIT_WIDTH_16BIT
#define AUDIO_RX_I2S_SLOT_MODE I2S_SLOT_MODE_STEREO

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
static audio_receiver_config_t g_config;
static bool g_initialized = false;
static audio_rx_state_t g_state = AUDIO_RX_STATE_IDLE;
static i2s_chan_handle_t g_i2s_tx_handle = NULL;
static i2c_master_bus_handle_t g_i2c_bus = NULL;
static i2c_master_dev_handle_t g_dac_dev = NULL;
static bool g_i2c_bus_owned = false;
static float g_i2s_gain = 0.5f;

// Stream ring buffer with critical section protection for dual-core safety
#define AUDIO_STREAM_RING_SIZE (262144)
static uint8_t *g_stream_ring = NULL;
static volatile size_t g_stream_write_pos = 0;
static volatile size_t g_stream_read_pos = 0;
static volatile size_t g_stream_available = 0;
static portMUX_TYPE g_stream_lock = portMUX_INITIALIZER_UNLOCKED;

#define STREAM_ENTER_CRITICAL() portENTER_CRITICAL(&g_stream_lock)
#define STREAM_EXIT_CRITICAL() portEXIT_CRITICAL(&g_stream_lock)

// Playback task resources
// Static stack in INTERNAL RAM — bypasses heap fragmentation and avoids PSRAM
// cache issues when ISRs touch the task stack.
#define AUDIO_PLAYBACK_STACK_BYTES 32768
static StackType_t g_playback_stack[AUDIO_PLAYBACK_STACK_BYTES];
static StaticTask_t g_playback_task_tcb;
static TaskHandle_t g_playback_task_handle = NULL;
static volatile bool g_stream_data_ready = false;

// Forward declarations
static esp_err_t dac_write_reg(uint8_t reg, uint8_t val);
static esp_err_t dac_set_page(uint8_t page);
static esp_err_t audio_rx_i2c_init(void);
static void audio_stream_callback(uint8_t channel, const uint8_t* data, size_t length, void* user_data);
static void audio_i2s_playback_task(void *arg);

esp_err_t audio_receiver_init(const audio_receiver_config_t *config)
{
    if (g_initialized) {
        ESP_LOGW(TAG, "Audio receiver already initialized");
        return ESP_OK;
    }

    if (!config) {
        ESP_LOGE(TAG, "Invalid configuration");
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(&g_config, config, sizeof(audio_receiver_config_t));
    g_i2s_gain = config->i2s_gain;

    ESP_LOGI(TAG, "Initializing audio receiver");
    ESP_LOGI(TAG, "I2S BCLK=%d, LRCLK=%d, DOUT=%d", g_config.i2s_bclk_pin, g_config.i2s_lrclk_pin, g_config.i2s_dout_pin);

    // Step 0: Allocate stream ring buffer from PSRAM (avoid .bss in DRAM)
    if (!g_stream_ring) {
        g_stream_ring = heap_caps_malloc(AUDIO_STREAM_RING_SIZE, MALLOC_CAP_SPIRAM);
        if (!g_stream_ring) {
            ESP_LOGE(TAG, "Failed to allocate stream ring buffer from PSRAM");
            return ESP_ERR_NO_MEM;
        }
    }

    // Step 1: Initialize I2S for output to DAC
    ESP_LOGI(TAG, "Initializing I2S...");
    esp_err_t ret = audio_rx_i2s_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Step 2: Initialize DAC via I2C (if pins configured)
    if (g_config.dac_i2c_sda_pin >= 0 && g_config.dac_i2c_scl_pin >= 0) {
        ESP_LOGI(TAG, "Initializing DAC via I2C...");
        ret = audio_rx_i2c_init();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "DAC I2C init failed (continuing without DAC control): %s", esp_err_to_name(ret));
        } else {
            ret = audio_rx_dac_init();
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "DAC init failed (continuing without DAC control): %s", esp_err_to_name(ret));
            }
        }
    } else {
        ESP_LOGI(TAG, "DAC I2C pins not configured, skipping DAC control");
    }

    // Step 3: Register GhostLink stream handler
    if (!esp_comm_manager_register_stream_handler(COMM_STREAM_CHANNEL_AUDIO, audio_stream_callback, NULL)) {
        ESP_LOGE(TAG, "Failed to register audio stream handler");
        return ESP_FAIL;
    }

    // Step 4: Create I2S playback task with xTaskCreateStatic
    // Static internal-RAM stack avoids heap fragmentation and PSRAM cache issues.
    g_playback_task_handle = xTaskCreateStatic(audio_i2s_playback_task, "audio_i2s_playback",
                                                AUDIO_PLAYBACK_STACK_BYTES, NULL, 4,
                                                g_playback_stack, &g_playback_task_tcb);
    if (!g_playback_task_handle) {
        ESP_LOGE(TAG, "Failed to create playback task (xTaskCreateStatic returned NULL)");
        if (g_i2s_tx_handle) {
            i2s_channel_disable(g_i2s_tx_handle);
            i2s_del_channel(g_i2s_tx_handle);
            g_i2s_tx_handle = NULL;
        }
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Playback task created, stack=%d bytes (internal RAM)", AUDIO_PLAYBACK_STACK_BYTES);

    g_initialized = true;
    g_state = AUDIO_RX_STATE_IDLE;
    ESP_LOGI(TAG, "Audio receiver initialized successfully");

    return ESP_OK;
}

esp_err_t audio_receiver_deinit(void)
{
    if (!g_initialized) {
        return ESP_OK;
    }

    if (g_playback_task_handle) {
        vTaskDelete(g_playback_task_handle);
        g_playback_task_handle = NULL;
    }

    if (g_stream_ring) {
        heap_caps_free(g_stream_ring);
        g_stream_ring = NULL;
    }

    if (g_i2s_tx_handle) {
        i2s_channel_disable(g_i2s_tx_handle);
        i2s_del_channel(g_i2s_tx_handle);
        g_i2s_tx_handle = NULL;
    }

    if (g_dac_dev) {
        i2c_master_bus_rm_device(g_dac_dev);
        g_dac_dev = NULL;
    }

    if (g_i2c_bus_owned && g_i2c_bus) {
        i2c_del_master_bus(g_i2c_bus);
        g_i2c_bus = NULL;
        g_i2c_bus_owned = false;
    }

    g_initialized = false;
    g_state = AUDIO_RX_STATE_IDLE;

    ESP_LOGI(TAG, "Audio receiver deinitialized");
    return ESP_OK;
}

bool audio_receiver_is_initialized(void)
{
    return g_initialized;
}

// I2C initialization for DAC control
static esp_err_t audio_rx_i2c_init(void)
{
    i2c_master_bus_config_t bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = g_config.dac_i2c_port,
        .scl_io_num = g_config.dac_i2c_scl_pin,
        .sda_io_num = g_config.dac_i2c_sda_pin,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    esp_err_t ret = i2c_new_master_bus(&bus_config, &g_i2c_bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    g_i2c_bus_owned = true;

    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = g_config.dac_i2c_addr,
        .scl_speed_hz = 100000,
        .scl_wait_us = 0,
    };

    ret = i2c_master_bus_add_device(g_i2c_bus, &dev_config, &g_dac_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C device add failed: %s", esp_err_to_name(ret));
        i2c_del_master_bus(g_i2c_bus);
        g_i2c_bus = NULL;
        g_i2c_bus_owned = false;
        return ret;
    }

    ESP_LOGI(TAG, "I2C initialized for DAC control at 0x%02X", g_config.dac_i2c_addr);
    return ESP_OK;
}

// DAC I2C write helper
static esp_err_t dac_write_reg(uint8_t reg, uint8_t val)
{
    if (!g_dac_dev) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t data[2] = {reg, val};
    return i2c_master_transmit(g_dac_dev, data, sizeof(data), 50);
}

// DAC set page
static esp_err_t dac_set_page(uint8_t page)
{
    return dac_write_reg(DAC_REG_PAGE_CTRL, page);
}

// DAC initialization sequence
esp_err_t audio_rx_dac_init(void)
{
    if (!g_dac_dev) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret;

    // Software reset
    ret = dac_set_page(0);
    if (ret != ESP_OK) return ret;
    ret = dac_write_reg(DAC_REG_SW_RESET, 0x01);
    if (ret != ESP_OK) return ret;
    vTaskDelay(pdMS_TO_TICKS(10));

    // Clock settings - use internal PLL with BCLK as reference
    // 0x06 = CODEC_CLKIN from PLL (01), PLL_CLKIN from BCLK (10)
    ret = dac_write_reg(DAC_REG_CLK_MUX, 0x06);
    if (ret != ESP_OK) return ret;

    // PLL configuration for 44.1kHz (BCLK = 1.4112 MHz)
    // 0x91 = PLL enabled, P=1, R=1
    ret = dac_write_reg(DAC_REG_CLK_PLL_PR, 0x91);
    if (ret != ESP_OK) return ret;
    ret = dac_write_reg(DAC_REG_CLK_PLL_J, 0x08);  // J=8 (matching Arduino)
    if (ret != ESP_OK) return ret;
    ret = dac_write_reg(DAC_REG_CLK_PLL_D_MSB, 0x00);  // D=0
    if (ret != ESP_OK) return ret;
    ret = dac_write_reg(DAC_REG_CLK_PLL_D_LSB, 0x00);
    if (ret != ESP_OK) return ret;

    // NDAC and MDAC (D7 = power bit) — matching reference firmware
    ret = dac_write_reg(DAC_REG_NDAC, 0x82);  // NDAC=2, powered
    if (ret != ESP_OK) return ret;
    ret = dac_write_reg(DAC_REG_MDAC, 0x82);  // MDAC=2, powered (matching Arduino)
    if (ret != ESP_OK) return ret;

    // DOSR (oversampling ratio) — 128 for 44.1kHz with J=64, NDAC=2, MDAC=8
    ret = dac_write_reg(DAC_REG_DOSR_MSB, 0x00);
    if (ret != ESP_OK) return ret;
    ret = dac_write_reg(DAC_REG_DOSR_LSB, 0x80);  // DOSR=128
    if (ret != ESP_OK) return ret;

    // Codec interface: I2S (Philips) mode, 16-bit, BCLK/WCLK inputs (slave)
    ret = dac_write_reg(DAC_REG_I2S_SETUP, 0x00);  // D7:D6=00(I2S/Philips), D5:D4=00(16b), D3=0(BCLK in), D2=0(WCLK in)
    if (ret != ESP_OK) return ret;

    // DAC processing block - PRB_P1 (simple stereo, no EQ, lowest latency)
    ret = dac_write_reg(DAC_REG_DAC_PRB, 0x01);
    if (ret != ESP_OK) return ret;

    // --- Page 1: Analog output routing and power (matching reference firmware) ---
    ret = dac_set_page(1);
    if (ret != ESP_OK) return ret;

    // LDO / Power setup
    ret = dac_write_reg(0x02, 0x09);  // AVDD LDO power up (initial)
    if (ret != ESP_OK) return ret;
    ret = dac_write_reg(0x01, 0x08);  // Disable weak AVDD (external supply)
    if (ret != ESP_OK) return ret;
    ret = dac_write_reg(0x02, 0x01);  // AVDD LDO powered, 1.72V
    if (ret != ESP_OK) return ret;

    // Common mode control: CM=0.9V, HP CM=1.65V, Line CM=1.65V, supplies from LDOin
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

    // Route DAC digital outputs to headphone drivers
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

    // Power up Class-D speaker amplifier (secondary output)
    ret = dac_write_reg(0x20, 0x80);  // SPK amp powered
    if (ret != ESP_OK) return ret;

    // --- Back to Page 0: Power up DAC datapath (AFTER analog path is ready) ---
    ret = dac_set_page(0);
    if (ret != ESP_OK) return ret;

    // 0xD6 = D7=1(L_pwr) D6=1(R_pwr) D5:D4=01(L_normal) D3:D2=01(R_normal) D1=1 D0=0
    ret = dac_write_reg(DAC_REG_DAC_DATAPATH, 0xD6);
    if (ret != ESP_OK) return ret;

    // Unmute DAC, independent volume control mode
    ret = dac_write_reg(DAC_REG_DAC_VOL_CTRL, 0x00);
    if (ret != ESP_OK) return ret;

    // Digital volume: 0dB full scale on both channels
    ret = dac_write_reg(DAC_REG_DAC_CH1_VOL, 0x00);
    if (ret != ESP_OK) return ret;
    ret = dac_write_reg(DAC_REG_DAC_CH2_VOL, 0x00);
    if (ret != ESP_OK) return ret;

    ESP_LOGI(TAG, "DAC TLV320DAC3100 initialized successfully");
    return ESP_OK;
}

esp_err_t audio_rx_dac_set_volume(uint8_t volume)
{
    if (!g_dac_dev) {
        return ESP_ERR_INVALID_STATE;
    }

    // Volume: 0 = mute, 1-255 = -63.5dB to 0dB (0.5dB steps)
    uint8_t dac_vol = volume >> 1;  // Convert 0-255 to 0-127
    if (volume == 0) {
        dac_vol = 0x80;  // Mute
    }

    esp_err_t ret = dac_write_reg(DAC_REG_DAC_CH1_VOL, dac_vol);
    if (ret != ESP_OK) return ret;
    ret = dac_write_reg(DAC_REG_DAC_CH2_VOL, dac_vol);
    return ret;
}

// I2S initialization
esp_err_t audio_rx_i2s_init(void)
{
    i2s_chan_config_t chan_config = I2S_CHANNEL_DEFAULT_CONFIG(AUDIO_RX_I2S_PORT, I2S_ROLE_MASTER);
    chan_config.auto_clear = false;

    ESP_LOGI(TAG, "Creating I2S TX channel on port %d", AUDIO_RX_I2S_PORT);
    esp_err_t ret = i2s_new_channel(&chan_config, &g_i2s_tx_handle, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S channel create failed: %s", esp_err_to_name(ret));
        return ret;
    }
    if (!g_i2s_tx_handle) {
        ESP_LOGE(TAG, "I2S TX channel handle is NULL after creation");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "I2S TX channel created successfully, handle=%p", (void*)g_i2s_tx_handle);

    i2s_std_config_t std_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_RX_I2S_SAMPLE_RATE),
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .ws_pol = false,
            .bit_shift = true,
        },
        .gpio_cfg = {
            .mclk = (g_config.i2s_mclk_pin >= 0) ? g_config.i2s_mclk_pin : I2S_GPIO_UNUSED,
            .bclk = g_config.i2s_bclk_pin,
            .ws = g_config.i2s_lrclk_pin,
            .dout = g_config.i2s_dout_pin,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    ret = i2s_channel_init_std_mode(g_i2s_tx_handle, &std_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S std mode init failed: %s", esp_err_to_name(ret));
        i2s_del_channel(g_i2s_tx_handle);
        g_i2s_tx_handle = NULL;
        return ret;
    }

    ret = i2s_channel_enable(g_i2s_tx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S channel enable failed: %s", esp_err_to_name(ret));
        i2s_del_channel(g_i2s_tx_handle);
        g_i2s_tx_handle = NULL;
        return ret;
    }

    // Boost GPIO drive strength for signal integrity over PCB traces / wires
    gpio_set_drive_capability(g_config.i2s_bclk_pin, GPIO_DRIVE_CAP_3);
    gpio_set_drive_capability(g_config.i2s_lrclk_pin, GPIO_DRIVE_CAP_3);
    gpio_set_drive_capability(g_config.i2s_dout_pin, GPIO_DRIVE_CAP_3);
    if (g_config.i2s_mclk_pin >= 0) {
        gpio_set_drive_capability(g_config.i2s_mclk_pin, GPIO_DRIVE_CAP_3);
    }

    ESP_LOGI(TAG, "I2S TX enabled: MCLK=%d, BCLK=%d, LRCLK=%d, DOUT=%d (drive=max)",
             g_config.i2s_mclk_pin, g_config.i2s_bclk_pin,
             g_config.i2s_lrclk_pin, g_config.i2s_dout_pin);
    return ESP_OK;
}

esp_err_t audio_rx_i2s_set_gain(float gain)
{
    if (gain < 0.0f) gain = 0.0f;
    if (gain > 1.0f) gain = 1.0f;
    g_i2s_gain = gain;
    ESP_LOGI(TAG, "I2S gain set to %.2f", gain);
    return ESP_OK;
}

// MP3 decoder state
static mp3dec_t g_mp3_dec;
static mp3dec_frame_info_t g_mp3_info;
static bool g_mp3_initialized = false;

static void audio_stream_callback(uint8_t channel, const uint8_t* data, size_t length, void* user_data)
{
    (void)channel;
    (void)user_data;

    if (length == 0) return;

    STREAM_ENTER_CRITICAL();
    size_t free_space = AUDIO_STREAM_RING_SIZE - g_stream_available;
    if (length > free_space) {
        length = free_space;
    }
    if (length == 0) {
        STREAM_EXIT_CRITICAL();
        return;
    }

    size_t write_pos = g_stream_write_pos;
    size_t first_part = AUDIO_STREAM_RING_SIZE - write_pos;
    if (first_part > length) first_part = length;
    memcpy(g_stream_ring + write_pos, data, first_part);
    if (first_part < length) {
        memcpy(g_stream_ring, data + first_part, length - first_part);
    }
    g_stream_write_pos = (write_pos + length) % AUDIO_STREAM_RING_SIZE;
    g_stream_available += length;
    STREAM_EXIT_CRITICAL();

    static uint32_t rx_chunk_count = 0;
    rx_chunk_count++;
    if ((rx_chunk_count & 0x1F) == 1) {
        ESP_LOGI(TAG, "RX chunk #%lu: %u bytes (ring: %u/%u)",
                 (unsigned long)rx_chunk_count, (unsigned)length,
                 (unsigned)g_stream_available, (unsigned)AUDIO_STREAM_RING_SIZE);
    }

    // One-time hex dump of first chunk for debugging
    static bool first_dump_done = false;
    if (!first_dump_done && length > 0) {
        first_dump_done = true;
        char hex_str[49];
        size_t dump_len = length < 16 ? length : 16;
        for (size_t i = 0; i < dump_len; i++) {
            snprintf(hex_str + i * 3, 4, "%02X ", data[i]);
        }
        ESP_LOGI(TAG, "First chunk hex dump (%u bytes): %s", (unsigned)dump_len, hex_str);
    }

    if (g_state != AUDIO_RX_STATE_PLAYING) {
        g_state = AUDIO_RX_STATE_PLAYING;
    }

    g_stream_data_ready = true;
}

// Fast scan for MP3 sync word with strict header validation.
// Rejects false sync words inside ID3 tags / JPEG album art by checking
// MPEG version, layer, bitrate index and sample-rate index.
// Returns the offset of the sync word, or len if not found.
static size_t find_mp3_sync_word(const uint8_t *buf, size_t len)
{
    for (size_t i = 0; i + 3 < len; i++) {
        if (buf[i] == 0xFF && (buf[i + 1] & 0xE0) == 0xE0) {
            uint8_t ver_layer = buf[i + 1] & 0x1E;
            // Reject reserved MPEG version (01) or reserved layer (00)
            if ((ver_layer & 0x18) == 0x08) continue;
            if ((ver_layer & 0x06) == 0x00) continue;

            uint8_t bitrate_sr = buf[i + 2];
            // Reject bad bitrate index (1111) or reserved sample-rate index (11)
            if ((bitrate_sr & 0xF0) == 0xF0) continue;
            if ((bitrate_sr & 0x0C) == 0x0C) continue;

            return i;
        }
    }
    return len;
}

static void audio_i2s_playback_task(void *arg)
{
    (void)arg;
    int16_t *pcm_buf = heap_caps_malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    uint8_t *mp3_buf = heap_caps_malloc(MINIMP3_MAX_FRAME_SIZE, MALLOC_CAP_SPIRAM);
    if (!pcm_buf || !mp3_buf) {
        ESP_LOGE(TAG, "Failed to allocate buffers");
        if (pcm_buf) heap_caps_free(pcm_buf);
        if (mp3_buf) heap_caps_free(mp3_buf);
        g_state = AUDIO_RX_STATE_ERROR;
        vTaskDelete(NULL);
        return;
    }

    // Warm-up FPU context: force a float operation so the coprocessor state
    // is active before minimp3's optimized DSP routines run.
    volatile float fpu_warmup = 1.0f;
    fpu_warmup = fpu_warmup * 1.5f + 0.5f;
    (void)fpu_warmup;

    if (!g_mp3_initialized) {
        mp3dec_init(&g_mp3_dec);
        g_mp3_initialized = true;
        ESP_LOGI(TAG, "MP3 decoder initialized");
    }

    ESP_LOGI(TAG, "Audio I2S playback task started, waiting for stream...");

    uint32_t frame_count = 0;
    uint32_t hunt_count = 0;
    bool first_frame = true;
    int detected_sample_rate = 0;

    // Minimum bytes before attempting decode.  A 320kbps MP3 frame is ~1045 bytes,
    // so we need at least 2× that to reliably find the next frame boundary.
    #define MIN_DECODE_BYTES 4096
    // Maximum bytes to hunt before giving up on this buffer segment.
    #define MAX_HUNT_BYTES     8192

    while (true) {
        STREAM_ENTER_CRITICAL();
        size_t available = g_stream_available;
        STREAM_EXIT_CRITICAL();

        // Wait for MIN_DECODE_BYTES before attempting decode, UNLESS the ring
        // buffer is nearly full — in that case, decode immediately to prevent
        // dropping incoming data.
        if (available < MIN_DECODE_BYTES && available < AUDIO_STREAM_RING_SIZE - 8192) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        if (!g_i2s_tx_handle) {
            ESP_LOGE(TAG, "I2S TX handle is NULL, cannot play audio");
            g_state = AUDIO_RX_STATE_ERROR;
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        g_state = AUDIO_RX_STATE_PLAYING;

        size_t mp3_read_size = available;
        if (mp3_read_size > MINIMP3_MAX_FRAME_SIZE) mp3_read_size = MINIMP3_MAX_FRAME_SIZE;

        // Copy from ring buffer into linear decode buffer in small chunks
        // to avoid holding the critical section for too long (blocks UART ISR).
        size_t copied = 0;
        size_t local_read_pos = 0;
        STREAM_ENTER_CRITICAL();
        local_read_pos = g_stream_read_pos;
        STREAM_EXIT_CRITICAL();
        while (copied < mp3_read_size) {
            size_t chunk = mp3_read_size - copied;
            if (chunk > 1024) chunk = 1024;
            size_t first_part = AUDIO_STREAM_RING_SIZE - local_read_pos;
            if (first_part > chunk) first_part = chunk;
            memcpy(mp3_buf + copied, g_stream_ring + local_read_pos, first_part);
            if (first_part < chunk) {
                memcpy(mp3_buf + copied + first_part, g_stream_ring, chunk - first_part);
            }
            local_read_pos = (local_read_pos + chunk) % AUDIO_STREAM_RING_SIZE;
            copied += chunk;
            if (copied < mp3_read_size) {
                taskYIELD();
            }
        }

        // Fast sync-word scan: find first MP3 frame header (0xFF 0xF?)
        size_t sync_offset = find_mp3_sync_word(mp3_buf, mp3_read_size);

        if (sync_offset > 0 && sync_offset < mp3_read_size) {
            // Sync word found after some non-MP3 data (e.g., ID3 tag).
            // Advance the ring buffer past the junk and try again next loop.
            STREAM_ENTER_CRITICAL();
            for (size_t i = 0; i < sync_offset; i++) {
                g_stream_read_pos = (g_stream_read_pos + 1) % AUDIO_STREAM_RING_SIZE;
                if (g_stream_available > 0) g_stream_available--;
            }
            STREAM_EXIT_CRITICAL();
            hunt_count = 0;
            if (sync_offset > 100) {
                ESP_LOGI(TAG, "Skipped %u bytes to sync word (ring: %u)",
                         (unsigned)sync_offset, (unsigned)g_stream_available);
            }
            continue;
        }

        if (sync_offset >= mp3_read_size) {
            // No sync word in the entire buffer — data is probably garbage.
            // Drop it and wait for fresh data.
            ESP_LOGW(TAG, "No MP3 sync word in %u bytes — dropping buffer", (unsigned)mp3_read_size);
            STREAM_ENTER_CRITICAL();
            g_stream_read_pos = g_stream_write_pos;
            g_stream_available = 0;
            STREAM_EXIT_CRITICAL();
            hunt_count = 0;
            continue;
        }

        // Sync word is at offset 0 in mp3_buf — attempt decode.
        int samples = mp3dec_decode_frame(&g_mp3_dec, mp3_buf, mp3_read_size, pcm_buf, &g_mp3_info);

        if (samples > 0 && g_mp3_info.frame_bytes > 0) {
            frame_count++;
            hunt_count = 0;

            if (first_frame) {
                detected_sample_rate = g_mp3_info.hz;
                ESP_LOGI(TAG, "First frame: sample_rate=%d Hz, channels=%d, bitrate=%d kbps",
                         detected_sample_rate, g_mp3_info.channels, g_mp3_info.bitrate_kbps);
                first_frame = false;
            }

            // Advance ring buffer by frame_offset + frame_bytes
            int total_consumed = g_mp3_info.frame_offset + g_mp3_info.frame_bytes;
            if (total_consumed < 0) total_consumed = g_mp3_info.frame_bytes;
            STREAM_ENTER_CRITICAL();
            for (int i = 0; i < total_consumed; i++) {
                g_stream_read_pos = (g_stream_read_pos + 1) % AUDIO_STREAM_RING_SIZE;
                if (g_stream_available > 0) g_stream_available--;
            }
            STREAM_EXIT_CRITICAL();

            size_t pcm_count = samples * g_mp3_info.channels;
            for (size_t i = 0; i < pcm_count; i++) {
                pcm_buf[i] = (int16_t)((float)pcm_buf[i] * g_i2s_gain);
            }

            size_t pcm_bytes = pcm_count * sizeof(int16_t);
            size_t bytes_written = 0;
            esp_err_t wr_ret = i2s_channel_write(g_i2s_tx_handle, pcm_buf, pcm_bytes, &bytes_written, pdMS_TO_TICKS(100));
            if (wr_ret != ESP_OK) {
                ESP_LOGE(TAG, "I2S write failed: %s (handle=%p, bytes=%u)", esp_err_to_name(wr_ret), (void*)g_i2s_tx_handle, (unsigned)pcm_bytes);
                if (wr_ret == ESP_ERR_INVALID_STATE) {
                    ESP_LOGE(TAG, "I2S channel in invalid state, attempting recovery");
                    i2s_channel_disable(g_i2s_tx_handle);
                    vTaskDelay(pdMS_TO_TICKS(10));
                    esp_err_t en_ret = i2s_channel_enable(g_i2s_tx_handle);
                    if (en_ret != ESP_OK) {
                        ESP_LOGE(TAG, "I2S channel re-enable failed: %s", esp_err_to_name(en_ret));
                    } else {
                        ESP_LOGI(TAG, "I2S channel recovered");
                    }
                } else if (wr_ret == ESP_ERR_INVALID_ARG) {
                    ESP_LOGE(TAG, "I2S write invalid arg - channel may be misconfigured");
                }
            } else if (bytes_written != pcm_bytes) {
                ESP_LOGW(TAG, "I2S partial write: expected %u, got %u", (unsigned)pcm_bytes, (unsigned)bytes_written);
            }

            if ((frame_count & 0x3F) == 1) {
                ESP_LOGI(TAG, "Decoded frame #%lu: %d Hz, %d samples, %d ch, consumed=%d, written=%u (ring: %u)",
                         (unsigned long)frame_count, g_mp3_info.hz, samples, g_mp3_info.channels,
                         total_consumed, (unsigned)pcm_bytes, (unsigned)g_stream_available);
            }
        } else {
            // Sync word found but decode failed — corrupted frame.  Advance past
            // the false sync word and yield CPU to let the UART parser catch up.
            hunt_count++;
            STREAM_ENTER_CRITICAL();
            g_stream_read_pos = (g_stream_read_pos + 1) % AUDIO_STREAM_RING_SIZE;
            if (g_stream_available > 0) g_stream_available--;
            STREAM_EXIT_CRITICAL();

            // Yield every failure to prevent starving the UART parser task.
            if ((hunt_count & 0x07) == 0) {
                vTaskDelay(pdMS_TO_TICKS(1));
            }

            // After 3 consecutive decode failures at sync words, reset decoder state
            // and skip ahead aggressively to get past corrupted regions faster.
            if (hunt_count >= 3) {
                ESP_LOGW(TAG, "Resetting MP3 decoder state after %lu failures (ring=%u)",
                         (unsigned long)hunt_count, (unsigned)g_stream_available);
                mp3dec_init(&g_mp3_dec);
                hunt_count = 0;

                // Skip 128 bytes to blast through the corrupted region (ID3 / JPEG data)
                size_t skip = 128;
                STREAM_ENTER_CRITICAL();
                if (skip > g_stream_available) skip = g_stream_available;
                for (size_t i = 0; i < skip; i++) {
                    g_stream_read_pos = (g_stream_read_pos + 1) % AUDIO_STREAM_RING_SIZE;
                    if (g_stream_available > 0) g_stream_available--;
                }
                STREAM_EXIT_CRITICAL();
                if (skip > 0) {
                    ESP_LOGI(TAG, "Skipped %u bytes after decoder reset", (unsigned)skip);
                }
                vTaskDelay(pdMS_TO_TICKS(1));
                continue;
            }
        }
    }
}

audio_rx_state_t audio_rx_get_state(void)
{
    return g_state;
}
