#include "cst816d.h"

#include <esp_log.h>
#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_shared.h"

#define TAG "CST816D"
#define I2C_MASTER_TIMEOUT_MS 1000
#define I2C_MASTER_FREQ_HZ 400000
#define I2C_ADDR_CST816D 0x15

static i2c_master_bus_handle_t s_cst816d_bus = NULL;
static i2c_master_dev_handle_t s_cst816d_dev = NULL;
static bool s_cst816d_bus_owned = false;

esp_err_t cst816d_i2c_read(uint8_t reg_addr, uint8_t *data, size_t len) {
    if (s_cst816d_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_master_transmit_receive(s_cst816d_dev, &reg_addr, 1, data, len, I2C_MASTER_TIMEOUT_MS);
}

esp_err_t cst816d_i2c_write(uint8_t reg_addr, uint8_t data) {
    if (s_cst816d_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t payload[2] = { reg_addr, data };
    return i2c_master_transmit(s_cst816d_dev, payload, sizeof(payload), I2C_MASTER_TIMEOUT_MS);
}

static void cst816d_reset_pins(void) {
    /* CST816-series panels hold the IC reset until RST is toggled low and
     * released high. Keep INT in input mode, only pulse RST. */
    if (CONFIG_LV_CST816D_INT >= 0 && GPIO_IS_VALID_GPIO(CONFIG_LV_CST816D_INT)) {
        gpio_config_t int_cfg = {
            .pin_bit_mask = BIT64(CONFIG_LV_CST816D_INT),
            .mode = GPIO_MODE_INPUT,
            .intr_type = GPIO_INTR_DISABLE,
            .pull_up_en = GPIO_PULLUP_ENABLE,
        };
        gpio_config(&int_cfg);
    }
    if (CONFIG_LV_CST816D_RST >= 0 && GPIO_IS_VALID_GPIO(CONFIG_LV_CST816D_RST)) {
        gpio_set_direction(CONFIG_LV_CST816D_RST, GPIO_MODE_OUTPUT);
        gpio_set_level(CONFIG_LV_CST816D_RST, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(CONFIG_LV_CST816D_RST, 1);
        vTaskDelay(pdMS_TO_TICKS(300));
    }
}

void cst816d_init(void) {
    ESP_ERROR_CHECK(i2c_shared_get_or_create_bus(CONFIG_LV_I2C_TOUCH_PORT,
                                                 CONFIG_LV_CST816D_SDA, CONFIG_LV_CST816D_SCL,
                                                 true, &s_cst816d_bus, &s_cst816d_bus_owned));

    cst816d_reset_pins();

    if (s_cst816d_dev == NULL) {
        /* Some panels carry an FT6x36-compatible IC at 0x38 instead of the
         * CST816D at 0x15. Probe both, they share the same register layout.
         * Retry a few times: the IC can take a moment to come up after the
         * reset pulse. */
        const uint16_t probe_addrs[] = { I2C_ADDR_CST816D, 0x38 };
        for (int attempt = 0; attempt < 3 && s_cst816d_dev == NULL; attempt++) {
            for (size_t i = 0; i < sizeof(probe_addrs) / sizeof(probe_addrs[0]); i++) {
                if (i2c_master_probe(s_cst816d_bus, probe_addrs[i], 100) == ESP_OK) {
                    esp_err_t ret = i2c_shared_add_device(s_cst816d_bus, probe_addrs[i],
                                                          I2C_MASTER_FREQ_HZ, &s_cst816d_dev);
                    if (ret == ESP_OK) {
                        ESP_LOGI(TAG, "Touch IC found at 0x%02X", probe_addrs[i]);
                        break;
                    }
                }
            }
            if (s_cst816d_dev == NULL && attempt < 2) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
        }
        if (s_cst816d_dev == NULL) {
            ESP_LOGE(TAG, "No touch IC detected on I2C bus (probed 0x15, 0x38)");
        }
    }

    if (s_cst816d_dev != NULL) {
        esp_err_t err = cst816d_i2c_write(0xFE, 0xFF);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Touch IC soft reset write failed: %s", esp_err_to_name(err));
        }
    }
}

bool cst816d_read(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    static int16_t last_x = 0;
    static int16_t last_y = 0;
    uint8_t touch_data[5] = {0};

    /* burst read: [num, x_h, x_l, y_h, y_l] */
    if (cst816d_i2c_read(0x02, touch_data, 5) != ESP_OK) {
        data->point.x = last_x;
        data->point.y = last_y;
        data->state = LV_INDEV_STATE_REL;
        return false;
    }

    uint8_t num = touch_data[0] & 0x0F;
    /* Chip serves 0xFF/0xF… garbage while idle; only accept clean frames. */
    if (!num || num > 1 || touch_data[0] == 0xFF || touch_data[2] == 0xFF) {
        data->point.x = last_x;
        data->point.y = last_y;
        data->state = LV_INDEV_STATE_REL;
        return false;
    }

    int16_t raw_x = ((touch_data[1] & 0x0f) << 8) | touch_data[2];
    int16_t raw_y = ((touch_data[3] & 0x0f) << 8) | touch_data[4];

    int16_t x = raw_x;
    int16_t y = raw_y;
#if CONFIG_LV_CST816D_SWAPXY
    int16_t temp = x;
    x = y;
    y = temp;
#endif
#if CONFIG_LV_CST816D_INVERT_X
    x = CONFIG_LV_HOR_RES_MAX - 1 - x;
#endif
#if CONFIG_LV_CST816D_INVERT_Y
    y = CONFIG_LV_VER_RES_MAX - 1 - y;
#endif

    last_x = x;
    last_y = y;
    data->point.x = x;
    data->point.y = y;
    data->state = LV_INDEV_STATE_PR;

    ESP_LOGI(TAG, "Touch: raw[%02X %02X %02X %02X %02X] x=%d, y=%d", touch_data[0], touch_data[1],
             touch_data[2], touch_data[3], touch_data[4], x, y);
    return false;
}
