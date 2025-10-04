#include "io_manager.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "i2c_bus_lock.h"

static const char *TAG = "IO_MANAGER";
static bool g_i2c_driver_installed = false; // whether this module installed the I2C driver
static SemaphoreHandle_t g_i2c_mutex = NULL;

// TCA9535 register addresses
#define TCA9535_INPUT_PORT0     0x00
#define TCA9535_INPUT_PORT1     0x01
#define TCA9535_OUTPUT_PORT0    0x02
#define TCA9535_OUTPUT_PORT1    0x03
#define TCA9535_CONFIG_PORT0    0x06
#define TCA9535_CONFIG_PORT1    0x07

// Button pin definitions (P00-P04)
#define BTN_UP      0   // P00
#define BTN_DOWN    1   // P01
#define BTN_SELECT  2   // P02
#define BTN_LEFT    3   // P03
#define BTN_RIGHT   4   // P04

// Debouncing constants
#define DEBOUNCE_MS 16

// Global variables
static io_manager_config_t g_config;
static bool g_initialized = false;
static uint8_t g_last_state = 0xFF;
static uint8_t g_stable_state = 0xFF;
static int64_t g_last_change_time = 0;

// Forward declarations
static esp_err_t tca9535_read_port(uint8_t reg, uint8_t *data);
static esp_err_t tca9535_write_port(uint8_t reg, uint8_t data);

esp_err_t io_manager_init(const io_manager_config_t *config)
{
    if (g_initialized) {
        ESP_LOGW(TAG, "IO Manager already initialized");
        return ESP_OK;
    }

    if (!config) {
        ESP_LOGE(TAG, "Invalid configuration");
        return ESP_ERR_INVALID_ARG;
    }

    // Copy configuration
    g_config = *config;

    // create mutex for i2c bus access
    if (!g_i2c_mutex) {
        g_i2c_mutex = xSemaphoreCreateMutex();
        if (!g_i2c_mutex) {
            ESP_LOGE(TAG, "failed to create i2c mutex");
            return ESP_ERR_NO_MEM;
        }
    }

    // Configure/Install I2C
    i2c_config_t i2c_conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = g_config.sda_pin,
        .scl_io_num = g_config.scl_pin,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,  // 100 kHz - standard i2c speed
    };

    // Try to install the driver first; if it's already installed, skip param_config
    esp_err_t ret = i2c_driver_install(g_config.i2c_port, I2C_MODE_MASTER, 0, 0, 0);
    if (ret == ESP_OK) {
        g_i2c_driver_installed = true;
        // We own the driver: safe to configure pins/clock
        ret = i2c_param_config(g_config.i2c_port, &i2c_conf);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to configure I2C parameters: %s", esp_err_to_name(ret));
            return ret;
        }
    } else if (ret == ESP_ERR_INVALID_STATE) {
        // Driver already installed elsewhere; share without reconfiguring
        ESP_LOGW(TAG, "I2C driver already installed on port %d; sharing", g_config.i2c_port);
        g_i2c_driver_installed = false;
    } else {
        ESP_LOGE(TAG, "Failed to install I2C driver: %s", esp_err_to_name(ret));
        return ret;
    }

    // Configure TCA9535 ports as inputs with pull-ups enabled
    ret = tca9535_write_port(TCA9535_CONFIG_PORT0, 0xFF);  // All pins as inputs
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure TCA9535 port 0: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = tca9535_write_port(TCA9535_CONFIG_PORT1, 0xFF);  // All pins as inputs
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure TCA9535 port 1: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Enable pull-ups on input pins (TCA9535 has internal pull-ups)
    ret = tca9535_write_port(TCA9535_OUTPUT_PORT0, 0xFF);  // Set output high to enable pull-ups
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable pull-ups on port 0: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ret = tca9535_write_port(TCA9535_OUTPUT_PORT1, 0xFF);  // Set output high to enable pull-ups
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable pull-ups on port 1: %s", esp_err_to_name(ret));
        return ret;
    }

    // Initialize state variables
    g_last_state = 0xFF;
    g_stable_state = 0xFF;
    g_last_change_time = 0;
    g_initialized = true;

    ESP_LOGI(TAG, "IO Manager initialized successfully");
    ESP_LOGI(TAG, "SDA: %d, SCL: %d, Address: 0x%02X", g_config.sda_pin, g_config.scl_pin, g_config.i2c_addr);

    return ESP_OK;
}

esp_err_t io_manager_deinit(void)
{
    if (!g_initialized) {
        return ESP_OK;
    }

    esp_err_t ret = ESP_OK;
    if (g_i2c_driver_installed) {
        ret = i2c_driver_delete(g_config.i2c_port);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to delete I2C driver: %s", esp_err_to_name(ret));
            return ret;
        }
    }

    if (g_i2c_mutex) {
        vSemaphoreDelete(g_i2c_mutex);
        g_i2c_mutex = NULL;
    }

    g_initialized = false;
    ESP_LOGI(TAG, "IO Manager deinitialized");

    return ESP_OK;
}

esp_err_t io_manager_scan_buttons(btn_event_t *event)
{
    if (!g_initialized || !event) {
        return ESP_ERR_INVALID_STATE;
    }

    // Read current port state
    uint8_t current_state;
    esp_err_t ret = tca9535_read_port(TCA9535_INPUT_PORT0, &current_state);
    if (ret != ESP_OK) {
        static int64_t last_log_ms = 0;
        int64_t now_ms = esp_timer_get_time() / 1000;
        if (now_ms - last_log_ms > 500) {
            ESP_LOGE(TAG, "Failed to read port state: %s", esp_err_to_name(ret));
            last_log_ms = now_ms;
        }
        return ret;
    }
    
    // Debug: Log raw port state
    ESP_LOGD(TAG, "Raw port state: 0x%02X (bit 0=up, bit 1=down, bit 2=select, bit 3=left, bit 4=right)", current_state);

    // Check if state changed
    if (current_state != g_last_state) {
        g_last_state = current_state;
        g_last_change_time = esp_timer_get_time() / 1000;  // Convert to milliseconds
    }

    // Debounce logic
    int64_t current_time = esp_timer_get_time() / 1000;
    if ((current_time - g_last_change_time) >= DEBOUNCE_MS && current_state != g_stable_state) {
        uint8_t prev_state = g_stable_state;
        g_stable_state = current_state;

        // Check for button presses (buttons are active low, so we check for 1->0 transitions)
        event->up = ((prev_state & (1 << BTN_UP)) && !(g_stable_state & (1 << BTN_UP)));
        event->down = ((prev_state & (1 << BTN_DOWN)) && !(g_stable_state & (1 << BTN_DOWN)));
        event->select = ((prev_state & (1 << BTN_SELECT)) && !(g_stable_state & (1 << BTN_SELECT)));
        event->left = ((prev_state & (1 << BTN_LEFT)) && !(g_stable_state & (1 << BTN_LEFT)));
        event->right = ((prev_state & (1 << BTN_RIGHT)) && !(g_stable_state & (1 << BTN_RIGHT)));
        
        // Debug logging
        ESP_LOGI(TAG, "Button state change - prev: 0x%02X, current: 0x%02X", prev_state, g_stable_state);
        ESP_LOGI(TAG, "Events - up: %d, down: %d, select: %d, left: %d, right: %d", 
                 event->up, event->down, event->select, event->left, event->right);
    } else {
        // No change, clear events
        event->up = false;
        event->down = false;
        event->select = false;
        event->left = false;
        event->right = false;
    }

    return ESP_OK;
}

esp_err_t io_manager_get_button_states(btn_event_t *states)
{
    if (!g_initialized || !states) {
        return ESP_ERR_INVALID_STATE;
    }

    // Read current port state
    uint8_t current_state;
    esp_err_t ret = tca9535_read_port(TCA9535_INPUT_PORT0, &current_state);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read port state: %s", esp_err_to_name(ret));
        return ret;
    }

    // Convert raw state to button states (buttons are active low)
    states->up = !(current_state & (1 << BTN_UP));
    states->down = !(current_state & (1 << BTN_DOWN));
    states->select = !(current_state & (1 << BTN_SELECT));
    states->left = !(current_state & (1 << BTN_LEFT));
    states->right = !(current_state & (1 << BTN_RIGHT));

    return ESP_OK;
}

uint8_t io_manager_get_raw_state(void)
{
    if (!g_initialized) {
        return 0xFF;
    }
    return g_stable_state;
}

// Debug function to get current button states
void io_manager_debug_states(void)
{
    if (!g_initialized) {
        ESP_LOGW(TAG, "IO Manager not initialized");
        return;
    }
    
    ESP_LOGI(TAG, "Debug - Last state: 0x%02X, Stable state: 0x%02X", g_last_state, g_stable_state);
    ESP_LOGI(TAG, "Debug - Up: %s, Down: %s, Select: %s, Left: %s, Right: %s",
             (g_stable_state & (1 << BTN_UP)) ? "HIGH" : "LOW",
             (g_stable_state & (1 << BTN_DOWN)) ? "HIGH" : "LOW", 
             (g_stable_state & (1 << BTN_SELECT)) ? "HIGH" : "LOW",
             (g_stable_state & (1 << BTN_LEFT)) ? "HIGH" : "LOW",
             (g_stable_state & (1 << BTN_RIGHT)) ? "HIGH" : "LOW");
}

void io_manager_reset_events(void)
{
    g_last_state = 0xFF;
    g_stable_state = 0xFF;
    g_last_change_time = 0;
}

// Helper function to read from TCA9535 register
static esp_err_t tca9535_read_port(uint8_t reg, uint8_t *data)
{
    if (!data) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!g_i2c_mutex) {
        return ESP_ERR_INVALID_STATE;
    }

    // acquire mutex with timeout to prevent deadlock
    if (xSemaphoreTake(g_i2c_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        ESP_LOGW(TAG, "failed to acquire i2c mutex for read");
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_FAIL;
    
    for (int attempt = 0; attempt < 2; attempt++) {
        if (attempt > 0) {
            vTaskDelay(pdMS_TO_TICKS(2));
        }
        bool global_locked = i2c_bus_lock(g_config.i2c_port, 60);
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        if (!cmd) {
            ret = ESP_ERR_NO_MEM;
            if (global_locked) i2c_bus_unlock(g_config.i2c_port);
            break;
        }
        
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (g_config.i2c_addr << 1) | I2C_MASTER_WRITE, true);
        i2c_master_write_byte(cmd, reg, true);
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (g_config.i2c_addr << 1) | I2C_MASTER_READ, true);
        i2c_master_read_byte(cmd, data, I2C_MASTER_NACK);
        i2c_master_stop(cmd);
        
        ret = i2c_master_cmd_begin(g_config.i2c_port, cmd, pdMS_TO_TICKS(50));
        i2c_cmd_link_delete(cmd);
        if (global_locked) i2c_bus_unlock(g_config.i2c_port);
        
        if (ret == ESP_OK) {
            break;
        }
    }

    xSemaphoreGive(g_i2c_mutex);
    return ret;
}

// Helper function to write to TCA9535 register
static esp_err_t tca9535_write_port(uint8_t reg, uint8_t data)
{
    if (!g_i2c_mutex) {
        return ESP_ERR_INVALID_STATE;
    }

    // acquire mutex with timeout to prevent deadlock
    if (xSemaphoreTake(g_i2c_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        ESP_LOGW(TAG, "failed to acquire i2c mutex for write");
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_FAIL;
    
    for (int attempt = 0; attempt < 2; attempt++) {
        if (attempt > 0) {
            vTaskDelay(pdMS_TO_TICKS(2));
        }
        bool global_locked = i2c_bus_lock(g_config.i2c_port, 60);
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        if (!cmd) {
            ret = ESP_ERR_NO_MEM;
            if (global_locked) i2c_bus_unlock(g_config.i2c_port);
            break;
        }
        
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (g_config.i2c_addr << 1) | I2C_MASTER_WRITE, true);
        i2c_master_write_byte(cmd, reg, true);
        i2c_master_write_byte(cmd, data, true);
        i2c_master_stop(cmd);
        
        ret = i2c_master_cmd_begin(g_config.i2c_port, cmd, pdMS_TO_TICKS(50));
        i2c_cmd_link_delete(cmd);
        if (global_locked) i2c_bus_unlock(g_config.i2c_port);
        
        if (ret == ESP_OK) {
            break;
        }
    }

    xSemaphoreGive(g_i2c_mutex);
    return ret;
}
