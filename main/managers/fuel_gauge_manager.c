#include "managers/fuel_gauge_manager.h"
#include "esp_log.h"
#include "driver/i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

#ifdef CONFIG_USE_BQ27220_FUEL_GAUGE

static const char *TAG = "FuelGaugeManager";

// BQ27220 I2C Configuration
#define I2C_MASTER_NUM          I2C_NUM_0
#define I2C_MASTER_FREQ_HZ      100000
#define I2C_MASTER_TX_BUF_DISABLE 0
#define I2C_MASTER_RX_BUF_DISABLE 0
#define I2C_MASTER_TIMEOUT_MS   1000

// BQ27220 Register Addresses (corrected for BQ27220)
#define BQ27220_REG_VOLTAGE     0x08  // Voltage in mV
#define BQ27220_REG_CURRENT     0x0C  // Current in mA
#define BQ27220_REG_SOC         0x2C  // State of Charge (%)
#define BQ27220_REG_CAPACITY    0x0E  // Full Charge Capacity in mAh
#define BQ27220_REG_REMAINING   0x10  // Remaining Capacity in mAh
#define BQ27220_REG_FLAGS       0x0A  // Battery status flags

// BQ27220 Flags (from datasheet)
#define BQ27220_FLAG_DSG        (1 << 0)   // Discharging flag
#define BQ27220_FLAG_SOCF       (1 << 1)   // State of Charge Final flag
#define BQ27220_FLAG_SOC1       (1 << 2)   // State of Charge 1 flag
#define BQ27220_FLAG_CHG        (1 << 8)   // Charging flag
#define BQ27220_FLAG_FC         (1 << 9)   // Full Charge flag
#define BQ27220_FLAG_OTD        (1 << 14)  // Over Temperature Discharge
#define BQ27220_FLAG_OTC        (1 << 15)  // Over Temperature Charge

// BQ27220 Control Commands
#define BQ27220_REG_CONTROL     0x00  // Control register
#define BQ27220_CONTROL_DEVICE_TYPE 0x0001  // Device type command

// Battery Configuration
#define BATTERY_DESIGN_CAPACITY CONFIG_BQ27220_DESIGN_CAPACITY  // mAh from configuration

static bool is_initialized = false;
static fuel_gauge_data_t last_data = {0};

/**
 * @brief Read 16-bit register from BQ27220
 */
static esp_err_t bq27220_read_reg16(uint8_t reg, uint16_t *value) {
    uint8_t data[2] = {0};
    esp_err_t ret;
    
    ret = i2c_master_write_read_device(I2C_MASTER_NUM, CONFIG_BQ27220_I2C_ADDRESS,
                                       &reg, 1, data, 2, pdMS_TO_TICKS(I2C_MASTER_TIMEOUT_MS));
    
    if (ret == ESP_OK) {
        *value = (data[1] << 8) | data[0];  // Little endian
        ESP_LOGD(TAG, "Read reg 0x%02X: raw bytes [0x%02X, 0x%02X] = %d", 
                 reg, data[0], data[1], *value);
    } else {
        ESP_LOGD(TAG, "Failed to read reg 0x%02X: %s", reg, esp_err_to_name(ret));
        *value = 0;
    }
    
    return ret;
}

/**
 * @brief Initialize I2C for BQ27220
 */
static esp_err_t bq27220_i2c_init(void) {
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = CONFIG_BQ27220_I2C_SDA_PIN,
        .scl_io_num = CONFIG_BQ27220_I2C_SCL_PIN,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
    };
    
    esp_err_t ret = i2c_param_config(I2C_MASTER_NUM, &conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C param config failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ret = i2c_driver_install(I2C_MASTER_NUM, conf.mode, 
                            I2C_MASTER_RX_BUF_DISABLE, 
                            I2C_MASTER_TX_BUF_DISABLE, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C driver install failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "I2C initialized - SDA: %d, SCL: %d", 
             CONFIG_BQ27220_I2C_SDA_PIN, CONFIG_BQ27220_I2C_SCL_PIN);
    
    return ESP_OK;
}

/**
 * @brief Scan I2C bus for devices (debugging helper)
 */
static void bq27220_scan_i2c_bus(void) {
    ESP_LOGI(TAG, "Scanning I2C bus...");
    int devices_found = 0;
    
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        uint8_t dummy_reg = 0x00;
        uint8_t dummy_data;
        
        esp_err_t ret = i2c_master_write_read_device(I2C_MASTER_NUM, addr,
                                                     &dummy_reg, 1, &dummy_data, 1, 
                                                     pdMS_TO_TICKS(50));
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Found device at address 0x%02X", addr);
            devices_found++;
        }
    }
    
    if (devices_found == 0) {
        ESP_LOGW(TAG, "No I2C devices found on the bus");
    } else {
        ESP_LOGI(TAG, "Found %d I2C device(s)", devices_found);
    }
}

/**
 * @brief Test if device is present on I2C bus by attempting a simple read
 */
static bool bq27220_device_present(void) {
    uint8_t dummy_reg = BQ27220_REG_VOLTAGE;
    uint8_t dummy_data[2];
    
    esp_err_t ret = i2c_master_write_read_device(I2C_MASTER_NUM, CONFIG_BQ27220_I2C_ADDRESS,
                                                 &dummy_reg, 1, dummy_data, 2, 
                                                 pdMS_TO_TICKS(100));
    
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "BQ27220 device detected at address 0x%02X", CONFIG_BQ27220_I2C_ADDRESS);
        return true;
    } else {
        ESP_LOGE(TAG, "No device found at address 0x%02X: %s", 
                 CONFIG_BQ27220_I2C_ADDRESS, esp_err_to_name(ret));
        
        // Perform I2C bus scan for debugging
        bq27220_scan_i2c_bus();
        return false;
    }
}

/**
 * @brief Check and configure design capacity if needed
 */
static bool bq27220_configure_capacity(void) {
    uint16_t current_capacity;
    esp_err_t ret = bq27220_read_reg16(BQ27220_REG_CAPACITY, &current_capacity);
    
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Could not read current design capacity");
        return true; // Continue anyway
    }
    
    ESP_LOGI(TAG, "Current design capacity: %d mAh", current_capacity);
    
    // If capacity is significantly different from expected, log a warning
    if (current_capacity < (BATTERY_DESIGN_CAPACITY - 200) || 
        current_capacity > (BATTERY_DESIGN_CAPACITY + 200)) {
        ESP_LOGW(TAG, "Design capacity (%d mAh) differs from expected (%d mAh)", 
                 current_capacity, BATTERY_DESIGN_CAPACITY);
        ESP_LOGW(TAG, "Consider reconfiguring the fuel gauge with correct capacity");
    }
    
    return true;
}

/**
 * @brief Test BQ27220 communication
 */
static bool bq27220_test_communication(void) {
    uint16_t voltage;
    esp_err_t ret = bq27220_read_reg16(BQ27220_REG_VOLTAGE, &voltage);
    
    ESP_LOGI(TAG, "Communication test - I2C result: %s, voltage reading: %d mV", 
             esp_err_to_name(ret), voltage);
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "BQ27220 I2C communication failed: %s", esp_err_to_name(ret));
        return false;
    }
    
    // BQ27220 can report 0V when battery is deeply discharged or not connected
    // Accept voltage range from 0 to 6000mV (reasonable for Li-ion batteries)
    if (voltage > 6000) {
        ESP_LOGW(TAG, "BQ27220 voltage reading seems high: %d mV, but continuing anyway", voltage);
    }
    
    ESP_LOGI(TAG, "BQ27220 communication test passed, voltage: %d mV", voltage);
    return true;
}

bool fuel_gauge_manager_init(void) {
    if (is_initialized) {
        ESP_LOGW(TAG, "Fuel gauge manager already initialized");
        return true;
    }
    
    ESP_LOGI(TAG, "Initializing BQ27220 fuel gauge manager");
    ESP_LOGI(TAG, "Configuration: Address=0x%02X, SDA=%d, SCL=%d", 
             CONFIG_BQ27220_I2C_ADDRESS, CONFIG_BQ27220_I2C_SDA_PIN, CONFIG_BQ27220_I2C_SCL_PIN);
    
    // Validate configuration
    if (CONFIG_BQ27220_I2C_SDA_PIN == CONFIG_BQ27220_I2C_SCL_PIN) {
        ESP_LOGE(TAG, "Invalid I2C configuration: SDA and SCL pins cannot be the same");
        return false;
    }
    
    // Initialize I2C
    esp_err_t ret = bq27220_i2c_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C initialization failed");
        return false;
    }
    
    // Small delay to allow I2C bus to stabilize
    vTaskDelay(pdMS_TO_TICKS(100));
    
    // Test device presence first
    if (!bq27220_device_present()) {
        i2c_driver_delete(I2C_MASTER_NUM);
        return false;
    }
    
    // Test communication
    if (!bq27220_test_communication()) {
        i2c_driver_delete(I2C_MASTER_NUM);
        return false;
    }
    
    // Check and configure design capacity
    if (!bq27220_configure_capacity()) {
        ESP_LOGW(TAG, "Failed to configure design capacity, continuing anyway");
    }
    
    // Initialize data structure
    memset(&last_data, 0, sizeof(last_data));
    last_data.is_initialized = true;
    
    is_initialized = true;
    ESP_LOGI(TAG, "BQ27220 fuel gauge manager initialized successfully");
    
    return true;
}

bool fuel_gauge_manager_get_data(fuel_gauge_data_t *data) {
    if (!is_initialized || !data) {
        return false;
    }
    
    uint16_t voltage, current_raw, soc, capacity, remaining, flags;
    
    // Read all registers with individual error checking
    esp_err_t ret_voltage = bq27220_read_reg16(BQ27220_REG_VOLTAGE, &voltage);
    esp_err_t ret_current = bq27220_read_reg16(BQ27220_REG_CURRENT, &current_raw);
    esp_err_t ret_soc = bq27220_read_reg16(BQ27220_REG_SOC, &soc);
    esp_err_t ret_capacity = bq27220_read_reg16(BQ27220_REG_CAPACITY, &capacity);
    esp_err_t ret_remaining = bq27220_read_reg16(BQ27220_REG_REMAINING, &remaining);
    esp_err_t ret_flags = bq27220_read_reg16(BQ27220_REG_FLAGS, &flags);
    
    if (ret_voltage != ESP_OK || ret_current != ESP_OK || ret_soc != ESP_OK ||
        ret_capacity != ESP_OK || ret_remaining != ESP_OK || ret_flags != ESP_OK) {
        
        ESP_LOGE(TAG, "Failed to read BQ27220 registers - V:%s C:%s SOC:%s CAP:%s REM:%s F:%s",
                 esp_err_to_name(ret_voltage), esp_err_to_name(ret_current),
                 esp_err_to_name(ret_soc), esp_err_to_name(ret_capacity),
                 esp_err_to_name(ret_remaining), esp_err_to_name(ret_flags));
        return false;
    }
    
    // Fill data structure
    data->voltage_mv = voltage;
    data->current_ma = (int16_t)current_raw;  // Signed value
    data->percentage = (uint8_t)(soc > 100 ? 100 : soc);
    data->capacity_mah = capacity;
    data->remaining_capacity_mah = remaining;
    
    // Improved charging detection
    // Check both the charging flag and current direction
    bool chg_flag = (flags & BQ27220_FLAG_CHG) != 0;
    bool positive_current = data->current_ma > 50;  // Positive current indicates charging (>50mA threshold)
    data->is_charging = chg_flag || positive_current;
    
    data->is_initialized = true;
    
    // Update cached data
    memcpy(&last_data, data, sizeof(fuel_gauge_data_t));
    
    ESP_LOGD(TAG, "Battery: %d%%, %dmV, %dmA, %dmAh/%dmAh, flags:0x%04X, %s", 
             data->percentage, data->voltage_mv, data->current_ma,
             data->remaining_capacity_mah, data->capacity_mah, flags,
             data->is_charging ? "charging" : "discharging");
    
    return true;
}

int fuel_gauge_manager_get_percentage(void) {
    fuel_gauge_data_t data;
    
    if (fuel_gauge_manager_get_data(&data)) {
        return data.percentage;
    }
    
    return -1;  // Error
}

bool fuel_gauge_manager_is_charging(void) {
    fuel_gauge_data_t data;
    
    if (fuel_gauge_manager_get_data(&data)) {
        return data.is_charging;
    }
    
    return false;
}

uint16_t fuel_gauge_manager_get_voltage_mv(void) {
    fuel_gauge_data_t data;
    
    if (fuel_gauge_manager_get_data(&data)) {
        return data.voltage_mv;
    }
    
    return 0;
}

void fuel_gauge_manager_deinit(void) {
    if (is_initialized) {
        i2c_driver_delete(I2C_MASTER_NUM);
        is_initialized = false;
        memset(&last_data, 0, sizeof(last_data));
        ESP_LOGI(TAG, "Fuel gauge manager deinitialized");
    }
}

#else // CONFIG_USE_BQ27220_FUEL_GAUGE not defined

// Stub implementations when fuel gauge is not enabled
bool fuel_gauge_manager_init(void) {
    return false;
}

bool fuel_gauge_manager_get_data(fuel_gauge_data_t *data) {
    return false;
}

int fuel_gauge_manager_get_percentage(void) {
    return -1;
}

bool fuel_gauge_manager_is_charging(void) {
    return false;
}

uint16_t fuel_gauge_manager_get_voltage_mv(void) {
    return 0;
}

void fuel_gauge_manager_deinit(void) {
    // Nothing to do
}

#endif // CONFIG_USE_BQ27220_FUEL_GAUGE