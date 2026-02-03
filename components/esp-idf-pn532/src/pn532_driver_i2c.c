#if defined(__has_include)
#  if __has_include(<string.h>)
#    include <string.h>
#  else
#    include <stddef.h>
     // minimal declarations for lint-only environments
     void *memcpy(void *dest, const void *src, size_t n);
     size_t strlen(const char *s);
#  endif
#else
#  include <string.h>
#endif
#include <stdbool.h>
#include "pn532_driver.h"
#include "pn532_driver_i2c.h"
#include "esp_log.h"
#include "i2c_bus_lock.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char TAG[] = "pn532_driver_i2c_legacy";

typedef struct {
    gpio_num_t sda;
    gpio_num_t scl;
    i2c_port_t i2c_port_number;
    uint8_t frame_buffer[256];
    bool owns_i2c_driver;
} pn532_i2c_driver_config;

static esp_err_t pn532_init_io(pn532_io_handle_t io_handle);
static void pn532_release_driver(pn532_io_handle_t io_handle);
static void pn532_release_io(pn532_io_handle_t io_handle);
static esp_err_t pn532_read(pn532_io_handle_t io_handle, uint8_t *read_buffer, size_t read_size, int xfer_timeout_ms);
static esp_err_t pn532_write(pn532_io_handle_t io_handle, const uint8_t *write_buffer, size_t write_size, int xfer_timeout_ms);
static esp_err_t pn532_is_ready(pn532_io_handle_t io_handle);
static bool pn532_check_bus_ok(gpio_num_t sda, gpio_num_t scl);
static bool pn532_quick_recovery(pn532_i2c_driver_config *config);
static esp_err_t pn532_i2c_transact_safe(i2c_port_t port, uint8_t addr,
                                         const uint8_t *tx, size_t tx_len,
                                         uint8_t *rx, size_t rx_len,
                                         int timeout_ms);

esp_err_t pn532_new_driver_i2c(gpio_num_t sda,
                               gpio_num_t scl,
                               gpio_num_t reset,
                               gpio_num_t irq,
                               i2c_port_t i2c_port_number,
                               pn532_io_handle_t io_handle)
{
    if (io_handle == NULL)
        return ESP_ERR_INVALID_ARG;

    pn532_i2c_driver_config *dev_config = heap_caps_calloc(1, sizeof(pn532_i2c_driver_config), MALLOC_CAP_DEFAULT);
    if (dev_config == NULL) {
        return ESP_ERR_NO_MEM;
    }

    io_handle->reset = reset;
    io_handle->irq = irq;

    dev_config->i2c_port_number = i2c_port_number;
    dev_config->scl = scl;
    dev_config->sda = sda;
    io_handle->driver_data = dev_config;

    io_handle->pn532_init_io = pn532_init_io;
    io_handle->pn532_release_io = pn532_release_io;
    io_handle->pn532_release_driver = pn532_release_driver;
    io_handle->pn532_read = pn532_read;
    io_handle->pn532_write = pn532_write;
    io_handle->pn532_init_extra = NULL;
    io_handle->pn532_is_ready = pn532_is_ready;

#ifdef CONFIG_ENABLE_IRQ_ISR
    io_handle->IRQQueue = NULL;
#endif

    return ESP_OK;
}

void pn532_release_driver(pn532_io_handle_t io_handle)
{
    if (io_handle == NULL || io_handle->driver_data == NULL)
        return;

    // Ensure IO is released (safe if already released)
    pn532_release_io(io_handle);
    free(io_handle->driver_data);
    io_handle->driver_data = NULL;
}

esp_err_t pn532_init_io(pn532_io_handle_t io_handle)
{
    if (io_handle == NULL || io_handle->driver_data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    pn532_i2c_driver_config *driver_config = (pn532_i2c_driver_config *)io_handle->driver_data;

    // BANSHEE V4: route is PN532 -> TCA953x -> ESP32; lower bus to 100 kHz for signal integrity
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = driver_config->sda,
        .scl_io_num = driver_config->scl,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .clk_flags = 0,
    };
    
#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
    if (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething") == 0) {
        conf.master.clk_speed = 100000;
    } else
#endif
    {
        conf.master.clk_speed = 400000;
    }

    driver_config->owns_i2c_driver = false;

    esp_err_t r = i2c_driver_install(driver_config->i2c_port_number, conf.mode, 0, 0, 0);
    if (r == ESP_OK) {
        driver_config->owns_i2c_driver = true;
        r = i2c_param_config(driver_config->i2c_port_number, &conf);
        if (r != ESP_OK) {
            ESP_LOGE(TAG, "i2c_param_config failed: 0x%x", (int)r);
            i2c_driver_delete(driver_config->i2c_port_number);
            driver_config->owns_i2c_driver = false;
            return r;
        }
#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
        // PN532 can hold SCL low for extended periods during RF operations (clock stretching)
        // Increase the I2C timeout to allow for this - max value for ESP32 is ~13ms per APB cycle
        // We set a high value to tolerate aggressive clock stretching
        if (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething") == 0) {
            i2c_set_timeout(driver_config->i2c_port_number, 0xFFFFF);  // Max timeout
            ESP_LOGI(TAG, "Configured extended I2C clock stretch timeout for PN532");
        }
#endif
    } else if (r == ESP_ERR_INVALID_STATE || r == ESP_FAIL) {
        driver_config->owns_i2c_driver = false;
        ESP_LOGW(TAG, "i2c_driver_install not owned by PN532 (already installed), proceeding with shared bus");
#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
        // Even on shared bus, try to increase timeout for PN532 clock stretching
        if (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething") == 0) {
            i2c_set_timeout(driver_config->i2c_port_number, 0xFFFFF);
        }
#endif
    } else {
        ESP_LOGE(TAG, "i2c_driver_install failed: 0x%x", (int)r);
        return r;
    }
    return ESP_OK;
}

void pn532_release_io(pn532_io_handle_t io_handle)
{
    if (io_handle == NULL || io_handle->driver_data == NULL) {
        return;
    }
    pn532_i2c_driver_config *driver_config = (pn532_i2c_driver_config *)io_handle->driver_data;
    if (driver_config->owns_i2c_driver) {
        esp_err_t r = i2c_driver_delete(driver_config->i2c_port_number);
        if (r != ESP_OK) {
            ESP_LOGW(TAG, "i2c_driver_delete failed: 0x%x", (int)r);
        }
        driver_config->owns_i2c_driver = false;
    }
}

// ---- somethingsomething template: bus health and stop-safe transact helpers ----

static bool pn532_check_bus_ok(gpio_num_t sda, gpio_num_t scl)
{
    return (gpio_get_level(sda) == 1) && (gpio_get_level(scl) == 1);
}

static bool pn532_nuclear_recovery(gpio_num_t sda, gpio_num_t scl, i2c_port_t port)
{
    ESP_LOGW(TAG, "Attempting nuclear I2C recovery - deleting and reinstalling driver");
    
    // Step 1: Delete I2C driver to reset the peripheral
    i2c_driver_delete(port);
    vTaskDelay(pdMS_TO_TICKS(20));
    
    // Step 2: Configure GPIOs as open-drain for manual bit-banging
    gpio_set_direction(scl, GPIO_MODE_INPUT_OUTPUT_OD);
    gpio_set_direction(sda, GPIO_MODE_INPUT_OUTPUT_OD);
    gpio_set_pull_mode(scl, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(sda, GPIO_PULLUP_ONLY);
    gpio_set_level(scl, 1);
    gpio_set_level(sda, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    
    // Step 3: Aggressive clock pulsing (32 cycles) to abort any stuck transaction
    for (int i = 0; i < 32; i++) {
        gpio_set_level(scl, 0);
        esp_rom_delay_us(10);
        gpio_set_level(scl, 1);
        esp_rom_delay_us(10);
    }
    
    // Step 4: Generate multiple STOP conditions
    for (int i = 0; i < 3; i++) {
        gpio_set_level(sda, 0);
        esp_rom_delay_us(10);
        gpio_set_level(scl, 1);
        esp_rom_delay_us(10);
        gpio_set_level(sda, 1);
        esp_rom_delay_us(10);
    }
    
    vTaskDelay(pdMS_TO_TICKS(20));
    
    // Step 5: Verify bus is now clear
    bool ok = (gpio_get_level(sda) == 1) && (gpio_get_level(scl) == 1);
    
    // Step 6: Reinstall I2C driver
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = sda,
        .scl_io_num = scl,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .clk_flags = 0,
    };
    
#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
    if (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething") == 0) {
        conf.master.clk_speed = 100000;  // Use slower speed after recovery
    } else
#endif
    {
        conf.master.clk_speed = 400000;
    }
    
    esp_err_t ret = i2c_driver_install(port, I2C_MODE_MASTER, 0, 0, 0);
    if (ret == ESP_OK || ret == ESP_ERR_INVALID_STATE) {
        i2c_param_config(port, &conf);
#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
        if (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething") == 0) {
            // i2c_set_timeout removed - relying on default or i2c_param_config sourcing
        }
#endif
    }
    
    if (ok) {
        ESP_LOGI(TAG, "Nuclear recovery successful - bus cleared");
    } else {
        ESP_LOGE(TAG, "Nuclear recovery failed - SCL=%d SDA=%d", 
                 gpio_get_level(scl), gpio_get_level(sda));
    }
    
    return ok;
}

static bool pn532_quick_recovery(pn532_i2c_driver_config *config)
{
    gpio_num_t sda = config->sda;
    gpio_num_t scl = config->scl;

    // First check if SCL is stuck low - this indicates either:
    // 1. PN532 is doing clock stretching (can last up to 2 seconds)
    // 2. ESP32 I2C peripheral is in an invalid state
    if (gpio_get_level(scl) == 0) {
        ESP_LOGW(TAG, "SCL stuck low, waiting for clock stretch to end...");
        // Wait up to 2 seconds for PN532 clock stretch to complete
        for (int i = 0; i < 40; i++) {  // 40 x 50ms = 2000ms max wait
            vTaskDelay(pdMS_TO_TICKS(50));
            if (gpio_get_level(scl) == 1) {
                ESP_LOGI(TAG, "SCL released after %dms", (i + 1) * 50);
                break;
            }
        }
        
        // If SCL is still low after waiting, the ESP32 peripheral may be stuck
        if (gpio_get_level(scl) == 0) {
            ESP_LOGW(TAG, "SCL still held low - attempting nuclear recovery");
            return pn532_nuclear_recovery(sda, scl, config->i2c_port_number);
        }
    }

    // Standard GPIO recovery: Pulse SCL (bounded) and send a STOP
    gpio_set_direction(scl, GPIO_MODE_INPUT_OUTPUT_OD);
    gpio_set_direction(sda, GPIO_MODE_INPUT_OUTPUT_OD);
    gpio_set_pull_mode(scl, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(sda, GPIO_PULLUP_ONLY);
    gpio_set_level(scl, 1);
    gpio_set_level(sda, 1);

    vTaskDelay(pdMS_TO_TICKS(1));

    for (int i = 0; i < 18; i++) {
        gpio_set_level(scl, 0);
        esp_rom_delay_us(5);
        gpio_set_level(scl, 1);
        esp_rom_delay_us(5);
        if (gpio_get_level(sda) == 1) break;
    }

    vTaskDelay(pdMS_TO_TICKS(1));

    gpio_set_level(sda, 0);
    esp_rom_delay_us(5);
    gpio_set_level(sda, 1);
    esp_rom_delay_us(5);

    bool ok = (gpio_get_level(sda) == 1) && (gpio_get_level(scl) == 1);

    vTaskDelay(pdMS_TO_TICKS(5));

    if (!ok) {
        ESP_LOGE(TAG, "Recovery FAILED - bus still stuck (SDA=%d SCL=%d)",
                 gpio_get_level(sda), gpio_get_level(scl));
    } else {
        ESP_LOGI(TAG, "Recovery OK");
    }

    return ok;
}

static esp_err_t pn532_i2c_transact_safe(i2c_port_t port, uint8_t addr,
                                         const uint8_t *tx, size_t tx_len,
                                         uint8_t *rx, size_t rx_len,
                                         int timeout_ms)
{
    esp_err_t ret;

    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if (!cmd) return ESP_ERR_NO_MEM;

    i2c_master_start(cmd);

    if (tx && tx_len > 0) {
        i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
        i2c_master_write(cmd, tx, tx_len, true);
    }

    if (rx && rx_len > 0) {
        if (tx && tx_len > 0) {
            i2c_master_start(cmd);
        }
        i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_READ, true);
        if (rx_len > 1) {
            i2c_master_read(cmd, rx, rx_len - 1, I2C_MASTER_ACK);
        }
        i2c_master_read_byte(cmd, rx + rx_len - 1, I2C_MASTER_NACK);
    }

    i2c_master_stop(cmd);

    ret = i2c_master_cmd_begin(port, cmd, pdMS_TO_TICKS(timeout_ms));
    i2c_cmd_link_delete(cmd);

    if (ret != ESP_OK) {
        i2c_cmd_handle_t stop_cmd = i2c_cmd_link_create();
        if (stop_cmd) {
            i2c_master_stop(stop_cmd);
            i2c_master_cmd_begin(port, stop_cmd, pdMS_TO_TICKS(10));
            i2c_cmd_link_delete(stop_cmd);
        }
        // Reset FIFOs to clear any corrupted peripheral state
        i2c_reset_tx_fifo(port);
        i2c_reset_rx_fifo(port);
    }

    return ret;
}

esp_err_t pn532_is_ready(pn532_io_handle_t io_handle)
{
    // Prefer IRQ when present; if IRQ not asserted, also check status byte over I2C
    if (io_handle->irq != GPIO_NUM_NC) {
        int level = gpio_get_level(io_handle->irq);
        if (level == 0) return ESP_OK;
        // Fallback: read PN532 status byte (0x01 means ready)
    }
    
#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
    if (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething") == 0) {
        // Short-timeout polling approach for somethingsomething to reduce bus lock time
        pn532_i2c_driver_config *driver_config = (pn532_i2c_driver_config *)io_handle->driver_data;
        
        for (int attempt = 0; attempt < 5; ++attempt) {
            if (attempt > 0) {
                // Yield between attempts to let other I2C consumers run
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            
            uint8_t status = 0;
            bool locked = i2c_bus_lock((int)driver_config->i2c_port_number, 50);
            if (!locked) return ESP_ERR_TIMEOUT;
            
            // Quick status read with short timeout
            esp_err_t res = i2c_master_read_from_device(driver_config->i2c_port_number, 0x24, &status, 1, 15 / portTICK_PERIOD_MS);
            i2c_bus_unlock((int)driver_config->i2c_port_number);
            
            if (res == ESP_OK && status == 0x01) {
                return ESP_OK;  // Ready
            }
            if (res != ESP_OK && res != ESP_ERR_TIMEOUT) {
                return res;  // Real error, not just timeout
            }
            // Continue to next attempt if not ready or timeout
        }
        return ESP_FAIL;  // Not ready after all attempts
    } else
#endif
    {
        // Original behavior for non-somethingsomething boards
        pn532_i2c_driver_config *driver_config = (pn532_i2c_driver_config *)io_handle->driver_data;
        uint8_t status = 0;
        bool locked = i2c_bus_lock((int)driver_config->i2c_port_number, 300);
        if (!locked) return ESP_ERR_TIMEOUT;
        esp_err_t res = i2c_master_read_from_device(driver_config->i2c_port_number, 0x24, &status, 1, 30 / portTICK_PERIOD_MS);
        i2c_bus_unlock((int)driver_config->i2c_port_number);
        if (res != ESP_OK) return res;
        return (status == 0x01) ? ESP_OK : ESP_FAIL;
    }
}

esp_err_t pn532_read(pn532_io_handle_t io_handle, uint8_t *read_buffer, size_t read_size, int xfer_timeout_ms)
{
    pn532_i2c_driver_config *driver_config = (pn532_i2c_driver_config *)io_handle->driver_data;
    if (!driver_config) return ESP_ERR_INVALID_ARG;
    uint8_t rx_buffer[256];
    if (read_size + 1 > sizeof(rx_buffer)) return ESP_ERR_INVALID_SIZE;
    int timeout_ms = (xfer_timeout_ms > 0) ? xfer_timeout_ms : 
#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
                     (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething") == 0 ? 100 : 100)
#else
                     100
#endif
                     ;

#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
    if (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething") == 0) {
        // Acquire lock FIRST to protect GPIO bus check/recovery from concurrent I2C access
        if (!i2c_bus_lock((int)driver_config->i2c_port_number, 150)) return ESP_ERR_TIMEOUT;

        if (!pn532_check_bus_ok(driver_config->sda, driver_config->scl)) {
            ESP_LOGW(TAG, "Bus stuck before read, recovering");
            pn532_quick_recovery(driver_config);
            vTaskDelay(pdMS_TO_TICKS(5));
            if (!pn532_check_bus_ok(driver_config->sda, driver_config->scl)) {
                ESP_LOGE(TAG, "Bus still stuck after recovery");
                i2c_bus_unlock((int)driver_config->i2c_port_number);
                return ESP_ERR_INVALID_STATE;
            }
        }

        esp_err_t res = pn532_i2c_transact_safe(driver_config->i2c_port_number, 0x24,
                                                NULL, 0,
                                                rx_buffer, read_size + 1,
                                                timeout_ms);
        i2c_bus_unlock((int)driver_config->i2c_port_number);
        if (res != ESP_OK) return res;
        if (rx_buffer[0] != 0x01) return ESP_ERR_TIMEOUT;
        memcpy(read_buffer, rx_buffer + 1, read_size);
        return ESP_OK;
    }
#endif

    int timeout = timeout_ms / portTICK_PERIOD_MS;
    bool locked = i2c_bus_lock((int)driver_config->i2c_port_number, 300);
    if (!locked) return ESP_ERR_TIMEOUT;
    esp_err_t res = i2c_master_read_from_device(driver_config->i2c_port_number, 0x24, rx_buffer, read_size + 1, timeout);
    i2c_bus_unlock((int)driver_config->i2c_port_number);
    if (res != ESP_OK) return res;
    if (rx_buffer[0] != 0x01) return ESP_ERR_TIMEOUT;
    memcpy(read_buffer, rx_buffer + 1, read_size);
    return ESP_OK;
}

esp_err_t pn532_write(pn532_io_handle_t io_handle, const uint8_t *write_buffer, size_t write_size, int xfer_timeout_ms)
{
    pn532_i2c_driver_config *driver_config = (pn532_i2c_driver_config *)io_handle->driver_data;
    if (!driver_config) return ESP_ERR_INVALID_ARG;
    // Ensure we don't overflow the local frame buffer (prefix + payload + suffix)
    if (write_size + 2 > sizeof(driver_config->frame_buffer)) {
        return ESP_ERR_INVALID_SIZE;
    }
    driver_config->frame_buffer[0] = 0;
    memcpy(driver_config->frame_buffer + 1, write_buffer, write_size);
    driver_config->frame_buffer[write_size + 1] = 0;
    int timeout_ms = (xfer_timeout_ms > 0) ? xfer_timeout_ms : 
#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
                     (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething") == 0 ? 100 : 100)
#else
                     100
#endif
                     ;

#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
    if (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething") == 0) {
        // Acquire lock FIRST to protect GPIO bus check/recovery from concurrent I2C access
        if (!i2c_bus_lock((int)driver_config->i2c_port_number, 150)) return ESP_ERR_TIMEOUT;

        if (!pn532_check_bus_ok(driver_config->sda, driver_config->scl)) {
            ESP_LOGW(TAG, "Bus stuck before write, recovering");
            pn532_quick_recovery(driver_config);
            vTaskDelay(pdMS_TO_TICKS(5));
            if (!pn532_check_bus_ok(driver_config->sda, driver_config->scl)) {
                ESP_LOGE(TAG, "Bus still stuck after recovery");
                i2c_bus_unlock((int)driver_config->i2c_port_number);
                return ESP_ERR_INVALID_STATE;
            }
        }

        esp_err_t r = pn532_i2c_transact_safe(driver_config->i2c_port_number, 0x24,
                                              driver_config->frame_buffer, write_size + 2,
                                              NULL, 0,
                                              timeout_ms);
        i2c_bus_unlock((int)driver_config->i2c_port_number);
        return r;
    }
#endif

    int timeout = timeout_ms / portTICK_PERIOD_MS;
    bool locked = i2c_bus_lock((int)driver_config->i2c_port_number, 300);
    if (!locked) return ESP_ERR_TIMEOUT;
    esp_err_t r = i2c_master_write_to_device(driver_config->i2c_port_number, 0x24, driver_config->frame_buffer, write_size + 2, timeout);
    i2c_bus_unlock((int)driver_config->i2c_port_number);
    return r;
}


