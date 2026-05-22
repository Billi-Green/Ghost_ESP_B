#include "managers/views/enviii_screen.h"
#include "managers/views/main_menu_screen.h"
#include "managers/display_manager.h"
#include "gui/screen_layout.h"
#include "gui/lvgl_safe.h"
#include "gui/theme_palette_api.h"
#include "managers/settings_manager.h"
#include "gui/accessibility_fonts.h"
#include "gui/toast.h"
#include "lvgl.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_bus_lock.h"
#include <math.h>

#ifdef CONFIG_HAS_ENVIII

static const char *TAG = "ENVIIIScreen";

static lv_obj_t *enviii_container = NULL;
static lv_obj_t *temp_label = NULL;
static lv_obj_t *hum_label = NULL;
static lv_obj_t *press_label = NULL;
static lv_obj_t *alt_label = NULL;
static lv_obj_t *status_label = NULL;
static lv_timer_t *enviii_timer = NULL;

static i2c_master_dev_handle_t s_sht30_dev = NULL;
static i2c_master_dev_handle_t s_qmp6988_dev = NULL;

static float sea_level_pressure = 1013.25f;
static bool sht_data_valid = false;
static bool qmp_data_valid = false;

#ifndef CONFIG_ENVIII_I2C_PORT
#define CONFIG_ENVIII_I2C_PORT 0
#endif
#define ENVIII_I2C_PORT CONFIG_ENVIII_I2C_PORT

#ifndef CONFIG_ENVIII_SHT30_I2C_ADDR
#define CONFIG_ENVIII_SHT30_I2C_ADDR 0x44
#endif
#define SHT30_I2C_ADDR CONFIG_ENVIII_SHT30_I2C_ADDR

#ifndef CONFIG_ENVIII_QMP6988_I2C_ADDR
#define CONFIG_ENVIII_QMP6988_I2C_ADDR 0x70
#endif
#define QMP6988_I2C_ADDR CONFIG_ENVIII_QMP6988_I2C_ADDR

/* -------------------------------------------------------------------------- */
/* SHT30                                                                      */
/* -------------------------------------------------------------------------- */
#define SHT30_CMD_MEASURE_HIGH 0x2C06

static uint8_t sht30_crc8(uint8_t *data, uint8_t len) {
    uint8_t crc = 0xFF;
    for (uint8_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t j = 0; j < 8; j++) {
            crc = (crc & 0x80) ? ((crc << 1) ^ 0x31) : (crc << 1);
        }
    }
    return crc;
}

static esp_err_t sht30_get_device(void) {
    if (s_sht30_dev) return ESP_OK;

    i2c_master_bus_handle_t bus = NULL;
    esp_err_t ret = i2c_master_get_bus_handle(ENVIII_I2C_PORT, &bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus %d unavailable: %s", ENVIII_I2C_PORT, esp_err_to_name(ret));
        return ret;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = SHT30_I2C_ADDR,
        .scl_speed_hz = 100000,
        .scl_wait_us = 0,
    };

    ret = i2c_master_bus_add_device(bus, &dev_cfg, &s_sht30_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add SHT30 device: %s", esp_err_to_name(ret));
    }
    return ret;
}

static esp_err_t sht30_write_cmd(uint16_t cmd) {
    esp_err_t ret = sht30_get_device();
    if (ret != ESP_OK) return ret;

    bool locked = i2c_bus_lock(ENVIII_I2C_PORT, 100);
    if (!locked) return ESP_ERR_TIMEOUT;

    uint8_t payload[2] = { (uint8_t)(cmd >> 8), (uint8_t)(cmd & 0xFF) };
    ret = i2c_master_transmit(s_sht30_dev, payload, sizeof(payload), 50);
    i2c_bus_unlock(ENVIII_I2C_PORT);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SHT30 write error: %s", esp_err_to_name(ret));
    }
    return ret;
}

static esp_err_t sht30_read_data(uint8_t *data, size_t len) {
    esp_err_t ret = sht30_get_device();
    if (ret != ESP_OK) return ret;

    bool locked = i2c_bus_lock(ENVIII_I2C_PORT, 100);
    if (!locked) return ESP_ERR_TIMEOUT;

    ret = i2c_master_receive(s_sht30_dev, data, len, 50);
    i2c_bus_unlock(ENVIII_I2C_PORT);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SHT30 read error: %s", esp_err_to_name(ret));
    }
    return ret;
}

static bool sht30_initialized = false;

static void sht30_init_hw(void) {
    if (sht30_initialized) return;
    sht30_write_cmd(0x30A2);  /* soft reset */
    vTaskDelay(pdMS_TO_TICKS(5));
    sht30_initialized = true;
    ESP_LOGI(TAG, "SHT30 initialized");
}

static bool sht30_read(float *out_temp, float *out_hum) {
    if (sht30_write_cmd(SHT30_CMD_MEASURE_HIGH) != ESP_OK) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(20));

    uint8_t data[6];
    if (sht30_read_data(data, sizeof(data)) != ESP_OK) {
        return false;
    }

    if (sht30_crc8(&data[0], 2) != data[2]) {
        ESP_LOGW(TAG, "SHT30 temp CRC mismatch");
    }
    if (sht30_crc8(&data[3], 2) != data[5]) {
        ESP_LOGW(TAG, "SHT30 hum CRC mismatch");
    }

    uint16_t raw_temp = (data[0] << 8) | data[1];
    uint16_t raw_hum  = (data[3] << 8) | data[4];

    *out_temp = -45.0f + 175.0f * ((float)raw_temp / 65535.0f);
    *out_hum  = 100.0f * ((float)raw_hum / 65535.0f);
    return true;
}

/* -------------------------------------------------------------------------- */
/* QMP6988  (based on M5Stack M5Unit-ENV driver)                            */
/* -------------------------------------------------------------------------- */
#define QMP6988_REG_CHIP_ID         0xD1
#define QMP6988_REG_RESET           0xE0
#define QMP6988_REG_CTRL_MEAS       0xF4
#define QMP6988_REG_CONFIG          0xF1
#define QMP6988_REG_PRESS_MSB       0xF7
#define QMP6988_REG_CALIBRATION     0xA0
#define QMP6988_CALIB_LEN           25
#define QMP6988_CHIP_ID_VAL         0x5C
#define QMP6988_SOFT_RESET_VAL      0xE6
#define QMP6988_SUBTRACTOR          8388608

typedef struct {
    int32_t  COE_a0;   /* 20Q4 */
    int16_t  COE_a1;
    int16_t  COE_a2;
    int32_t  COE_b00;  /* 20Q4 */
    int16_t  COE_bt1;
    int16_t  COE_bt2;
    int16_t  COE_bp1;
    int16_t  COE_b11;
    int16_t  COE_bp2;
    int16_t  COE_b12;
    int16_t  COE_b21;
    int16_t  COE_bp3;
} qmp6988_cali_data_t;

typedef struct {
    int32_t a0, b00;
    int32_t a1, a2;
    int64_t bt1, bt2, bp1, b11, bp2, b12, b21, bp3;
} qmp6988_ik_data_t;

static qmp6988_cali_data_t qmp_cali = {0};
static qmp6988_ik_data_t qmp_ik = {0};
static bool qmp6988_initialized = false;

static esp_err_t qmp6988_get_device(void) {
    if (s_qmp6988_dev) return ESP_OK;

    i2c_master_bus_handle_t bus = NULL;
    esp_err_t ret = i2c_master_get_bus_handle(ENVIII_I2C_PORT, &bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus %d unavailable: %s", ENVIII_I2C_PORT, esp_err_to_name(ret));
        return ret;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = QMP6988_I2C_ADDR,
        .scl_speed_hz = 100000,
        .scl_wait_us = 0,
    };

    ret = i2c_master_bus_add_device(bus, &dev_cfg, &s_qmp6988_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add QMP6988 device: %s", esp_err_to_name(ret));
    }
    return ret;
}

static esp_err_t qmp6988_write_reg(uint8_t reg, uint8_t val) {
    esp_err_t ret = qmp6988_get_device();
    if (ret != ESP_OK) return ret;

    bool locked = i2c_bus_lock(ENVIII_I2C_PORT, 100);
    if (!locked) return ESP_ERR_TIMEOUT;

    uint8_t payload[2] = {reg, val};
    ret = i2c_master_transmit(s_qmp6988_dev, payload, sizeof(payload), 50);
    i2c_bus_unlock(ENVIII_I2C_PORT);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "QMP6988 write error: %s", esp_err_to_name(ret));
    }
    return ret;
}

static esp_err_t qmp6988_read_bytes(uint8_t reg, uint8_t *data, size_t len) {
    esp_err_t ret = qmp6988_get_device();
    if (ret != ESP_OK) return ret;

    bool locked = i2c_bus_lock(ENVIII_I2C_PORT, 100);
    if (!locked) return ESP_ERR_TIMEOUT;

    ret = i2c_master_transmit_receive(s_qmp6988_dev, &reg, sizeof(reg), data, len, 50);
    i2c_bus_unlock(ENVIII_I2C_PORT);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "QMP6988 read error: %s", esp_err_to_name(ret));
    }
    return ret;
}

static void qmp6988_read_calibration(void) {
    uint8_t buf[32] = {0};
    if (qmp6988_read_bytes(QMP6988_REG_CALIBRATION, buf, QMP6988_CALIB_LEN) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read QMP6988 calibration");
        return;
    }

    /* M5Stack QMP6988 coefficient extraction (scattered across 25 bytes) */
    qmp_cali.COE_a0 = (int32_t)(((buf[18] << 12) | (buf[19] << 4) | (buf[24] & 0x0f)) << 12);
    qmp_cali.COE_a0 = qmp_cali.COE_a0 >> 12;

    qmp_cali.COE_a1  = (int16_t)((buf[20] << 8) | buf[21]);
    qmp_cali.COE_a2  = (int16_t)((buf[22] << 8) | buf[23]);

    qmp_cali.COE_b00 = (int32_t)(((buf[0] << 12) | (buf[1] << 4) | ((buf[24] & 0xf0) >> 4)) << 12);
    qmp_cali.COE_b00 = qmp_cali.COE_b00 >> 12;

    qmp_cali.COE_bt1 = (int16_t)((buf[2] << 8) | buf[3]);
    qmp_cali.COE_bt2 = (int16_t)((buf[4] << 8) | buf[5]);
    qmp_cali.COE_bp1 = (int16_t)((buf[6] << 8) | buf[7]);
    qmp_cali.COE_b11 = (int16_t)((buf[8] << 8) | buf[9]);
    qmp_cali.COE_bp2 = (int16_t)((buf[10] << 8) | buf[11]);
    qmp_cali.COE_b12 = (int16_t)((buf[12] << 8) | buf[13]);
    qmp_cali.COE_b21 = (int16_t)((buf[14] << 8) | buf[15]);
    qmp_cali.COE_bp3 = (int16_t)((buf[16] << 8) | buf[17]);

    ESP_LOGI(TAG, "QMP6988 cal: a0=%ld a1=%d a2=%d b00=%ld",
             qmp_cali.COE_a0, qmp_cali.COE_a1, qmp_cali.COE_a2, qmp_cali.COE_b00);
    ESP_LOGI(TAG, "QMP6988 cal: bt1=%d bt2=%d bp1=%d b11=%d bp2=%d b12=%d b21=%d bp3=%d",
             qmp_cali.COE_bt1, qmp_cali.COE_bt2, qmp_cali.COE_bp1, qmp_cali.COE_b11,
             qmp_cali.COE_bp2, qmp_cali.COE_b12, qmp_cali.COE_b21, qmp_cali.COE_bp3);

    /* Integer coefficient transforms (M5Stack driver) */
    qmp_ik.a0  = qmp_cali.COE_a0;   /* 20Q4 */
    qmp_ik.b00 = qmp_cali.COE_b00;  /* 20Q4 */

    qmp_ik.a1  = 3608L * (int32_t)qmp_cali.COE_a1 - 1731677965L;   /* 31Q23 */
    qmp_ik.a2  = 16889L * (int32_t)qmp_cali.COE_a2 - 87619360L;     /* 30Q47 */
    qmp_ik.bt1 = 2982L * (int64_t)qmp_cali.COE_bt1 + 107370906L;    /* 28Q15 */
    qmp_ik.bt2 = 329854L * (int64_t)qmp_cali.COE_bt2 + 108083093L;  /* 34Q38 */
    qmp_ik.bp1 = 19923L * (int64_t)qmp_cali.COE_bp1 + 1133836764L;  /* 31Q20 */
    qmp_ik.b11 = 2406L * (int64_t)qmp_cali.COE_b11 + 118215883L;    /* 28Q34 */
    qmp_ik.bp2 = 3079L * (int64_t)qmp_cali.COE_bp2 - 181579595L;    /* 29Q43 */
    qmp_ik.b12 = 6846L * (int64_t)qmp_cali.COE_b12 + 85590281L;     /* 29Q53 */
    qmp_ik.b21 = 13836L * (int64_t)qmp_cali.COE_b21 + 79333336L;     /* 29Q60 */
    qmp_ik.bp3 = 2915L * (int64_t)qmp_cali.COE_bp3 + 157155561L;    /* 28Q65 */
}

/* Temperature compensation: returns 8.8 fixed-point (divide by 256 for °C) */
static int16_t qmp6988_convTx02e(qmp6988_ik_data_t *ik, int32_t dt) {
    int64_t wk1, wk2;

    wk1 = ((int64_t)ik->a1 * (int64_t)dt);           /* 31Q23 + 24-1 = 54Q23 */
    wk2 = ((int64_t)ik->a2 * (int64_t)dt) >> 14;     /* 30Q47 + 24-1 = 53 -> 39Q33 */
    wk2 = (wk2 * (int64_t)dt) >> 10;                 /* 39Q33 + 24-1 = 62 -> 52Q23 */
    wk2 = ((wk1 + wk2) / 32767) >> 19;               /* 55Q23 -> 20Q04 */
    return (int16_t)((ik->a0 + wk2) >> 4);            /* 21Q4 -> 17Q0 (8.8) */
}

/* Pressure compensation: returns 20.4 fixed-point (divide by 16 for Pa) */
static int32_t qmp6988_getPressure02e(qmp6988_ik_data_t *ik, int32_t dp, int16_t tx) {
    int64_t wk1, wk2, wk3;

    wk1 = ((int64_t)ik->bt1 * (int64_t)tx);           /* 28Q15 + 16-1 = 43Q15 */
    wk2 = ((int64_t)ik->bp1 * (int64_t)dp) >> 5;      /* 31Q20 + 24-1 = 54 -> 49Q15 */
    wk1 += wk2;
    wk2 = ((int64_t)ik->bt2 * (int64_t)tx) >> 1;      /* 34Q38 + 16-1 = 49 -> 48Q37 */
    wk2 = (wk2 * (int64_t)tx) >> 8;                   /* 48Q37 + 16-1 = 63 -> 55Q29 */
    wk3 = wk2;
    wk2 = ((int64_t)ik->b11 * (int64_t)tx) >> 4;      /* 28Q34 + 16-1 = 43 -> 39Q30 */
    wk2 = (wk2 * (int64_t)dp) >> 1;                   /* 39Q30 + 24-1 = 62 -> 61Q29 */
    wk3 += wk2;
    wk2 = ((int64_t)ik->bp2 * (int64_t)dp) >> 13;     /* 29Q43 + 24-1 = 52 -> 39Q30 */
    wk2 = (wk2 * (int64_t)dp) >> 1;                   /* 39Q30 + 24-1 = 62 -> 61Q29 */
    wk3 += wk2;
    wk1 += wk3 >> 14;                                 /* Q29 >> 14 = Q15 */
    wk2 = ((int64_t)ik->b12 * (int64_t)tx);           /* 29Q53 + 16-1 = 45Q53 */
    wk2 = (wk2 * (int64_t)tx) >> 22;                  /* 45Q53 + 16-1 = 61 -> 39Q31 */
    wk2 = (wk2 * (int64_t)dp) >> 1;                   /* 39Q31 + 24-1 = 62 -> 61Q30 */
    wk3 = wk2;
    wk2 = ((int64_t)ik->b21 * (int64_t)tx) >> 6;      /* 29Q60 + 16-1 = 45 -> 39Q54 */
    wk2 = (wk2 * (int64_t)dp) >> 23;                  /* 39Q54 + 24-1 = 62 -> 39Q31 */
    wk2 = (wk2 * (int64_t)dp) >> 1;                   /* 39Q31 + 24-1 = 62 -> 61Q30 */
    wk3 += wk2;
    wk2 = ((int64_t)ik->bp3 * (int64_t)dp) >> 12;     /* 28Q65 + 24-1 = 51 -> 39Q53 */
    wk2 = (wk2 * (int64_t)dp) >> 23;                  /* 39Q53 + 24-1 = 62 -> 39Q30 */
    wk2 = (wk2 * (int64_t)dp);                        /* 39Q30 + 24-1 = 62Q30 */
    wk3 += wk2;
    wk1 += wk3 >> 15;                                 /* Q30 >> 15 = Q15 */
    wk1 /= 32767L;
    wk1 >>= 11;                                       /* Q15 >> 11 = Q4 */
    wk1 += ik->b00;                                   /* Q4 + 20Q4 */
    return (int32_t)wk1;
}

static void qmp6988_init_hw(void) {
    if (qmp6988_initialized) return;

    uint8_t chip_id = 0;
    if (qmp6988_read_bytes(QMP6988_REG_CHIP_ID, &chip_id, 1) != ESP_OK || chip_id != QMP6988_CHIP_ID_VAL) {
        ESP_LOGE(TAG, "QMP6988 not found (ID: 0x%02X)", chip_id);
        return;
    }
    ESP_LOGI(TAG, "Found QMP6988 ID: 0x%02X", chip_id);

    /* Soft reset */
    qmp6988_write_reg(QMP6988_REG_RESET, QMP6988_SOFT_RESET_VAL);
    vTaskDelay(pdMS_TO_TICKS(20));

    qmp6988_read_calibration();

    /* Normal mode, P oversampling 8x, T oversampling 1x, filter coeff 4 */
    qmp6988_write_reg(QMP6988_REG_CTRL_MEAS, 0x33);
    qmp6988_write_reg(QMP6988_REG_CONFIG, 0x02);
    vTaskDelay(pdMS_TO_TICKS(20));

    qmp6988_initialized = true;
    ESP_LOGI(TAG, "QMP6988 initialized");
}

static bool qmp6988_read(float *out_press_hpa, float *out_temp_c) {
    uint8_t data[6] = {0};
    if (qmp6988_read_bytes(QMP6988_REG_PRESS_MSB, data, 6) != ESP_OK) {
        return false;
    }

    uint32_t P_read = ((uint32_t)data[0] << 16) | ((uint32_t)data[1] << 8) | data[2];
    uint32_t T_read = ((uint32_t)data[3] << 16) | ((uint32_t)data[4] << 8) | data[5];

    int32_t P_raw = (int32_t)(P_read - QMP6988_SUBTRACTOR);
    int32_t T_raw = (int32_t)(T_read - QMP6988_SUBTRACTOR);

    int16_t T_int = qmp6988_convTx02e(&qmp_ik, T_raw);
    int32_t P_int = qmp6988_getPressure02e(&qmp_ik, P_raw, T_int);

    *out_temp_c = (float)T_int / 256.0f;
    *out_press_hpa = (float)P_int / 16.0f / 100.0f;  /* Pa -> hPa */
    return true;
}

/* -------------------------------------------------------------------------- */
/* UI Update                                                                  */
/* -------------------------------------------------------------------------- */
static bool sensors_ready_shown = false;
static float last_temp = 0.0f;
static float last_hum = 0.0f;
static float last_press = 0.0f;

static float apply_filter(float prev, float curr, float alpha) {
    return (prev * (1.0f - alpha)) + (curr * alpha);
}

static void enviii_timer_cb(lv_timer_t *timer) {
    (void)timer;

    float temp = 0.0f, hum = 0.0f, press = 0.0f, qmp_temp = 0.0f;
    bool sht_ok = false;
    bool qmp_ok = false;

    if (sht30_initialized) {
        sht_ok = sht30_read(&temp, &hum);
    }

    if (qmp6988_initialized) {
        qmp_ok = qmp6988_read(&press, &qmp_temp);
    }

    static uint32_t last_log = 0;
    uint32_t now = esp_timer_get_time() / 1000;
    bool do_log = (now - last_log > 3000);
    if (do_log) last_log = now;

    if (sht_ok) {
        sht_data_valid = true;
        last_temp = apply_filter(last_temp, temp, 0.3f);
        last_hum  = apply_filter(last_hum, hum, 0.3f);
        if (do_log) {
            ESP_LOGI(TAG, "SHT30 raw: T=%.2f C  H=%.2f %%", (double)temp, (double)hum);
        }
    } else if (do_log) {
        ESP_LOGW(TAG, "SHT30 read failed");
    }

    if (qmp_ok) {
        /* Sanity-check: real atmospheric pressure is 300-1100 hPa */
        if (press >= 300.0f && press <= 1100.0f) {
            qmp_data_valid = true;
            last_press = apply_filter(last_press, press, 0.3f);
            if (do_log) {
                ESP_LOGI(TAG, "QMP6988 raw: P=%.2f hPa  T=%.2f C", (double)press, (double)qmp_temp);
            }
        } else {
            ESP_LOGW(TAG, "QMP6988 pressure out of range: %.2f hPa (raw_press/T from regs)", (double)press);
        }
    } else if (do_log) {
        ESP_LOGW(TAG, "QMP6988 read failed");
    }

    if (sht_data_valid && qmp_data_valid && !sensors_ready_shown) {
        sensors_ready_shown = true;
        toast_show("ENV-III sensors ready", TOAST_SUCCESS);
    }

    char buf[64];

    if (temp_label) {
        if (sht_data_valid) {
            snprintf(buf, sizeof(buf), "%.1f C / %.1f F",
                     (double)last_temp, (double)(last_temp * 9.0f / 5.0f + 32.0f));
            lv_label_set_text(temp_label, buf);
        } else {
            lv_label_set_text(temp_label, "--.- C / --.- F");
        }
    }
    if (hum_label) {
        if (sht_data_valid) {
            snprintf(buf, sizeof(buf), "%.1f %%", (double)last_hum);
            lv_label_set_text(hum_label, buf);
        } else {
            lv_label_set_text(hum_label, "--.- %%");
        }
    }
    if (press_label) {
        if (qmp_data_valid) {
            snprintf(buf, sizeof(buf), "%.1f hPa / %.2f inHg",
                     (double)last_press, (double)(last_press * 0.02953f));
            lv_label_set_text(press_label, buf);
        } else {
            lv_label_set_text(press_label, "---.- hPa / --.-- inHg");
        }
    }
    if (alt_label) {
        if (qmp_data_valid) {
            float altitude = 44330.0f * (1.0f - powf(last_press / sea_level_pressure, 0.1903f));
            snprintf(buf, sizeof(buf), "%.0f m / %.0f ft",
                     (double)altitude, (double)(altitude * 3.28084f));
            lv_label_set_text(alt_label, buf);
        } else {
            lv_label_set_text(alt_label, "--- m / --- ft");
        }
    }

    if (status_label) {
        if (sht_ok && qmp_ok) {
            lv_label_set_text(status_label, "SHT30 OK | QMP6988 OK");
            lv_obj_set_style_text_color(status_label, lv_color_hex(0x4CAF50), 0);
        } else if (!sht_ok && !qmp_ok) {
            lv_label_set_text(status_label, "Sensors not detected");
            lv_obj_set_style_text_color(status_label, lv_color_hex(0xF44336), 0);
        } else {
            snprintf(buf, sizeof(buf), "%s | %s",
                     sht_ok ? "SHT30 OK" : "SHT30 ERR",
                     qmp_ok ? "QMP6988 OK" : "QMP6988 ERR");
            lv_label_set_text(status_label, buf);
            lv_obj_set_style_text_color(status_label, lv_color_hex(0xFF9800), 0);
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Input Handling                                                             */
/* -------------------------------------------------------------------------- */
static void enviii_event_handler(InputEvent *event) {
    if (event->type == INPUT_TYPE_TOUCH && event->data.touch_data.state == LV_INDEV_STATE_PR) {
        if (qmp_data_valid) {
            sea_level_pressure = last_press;
            toast_show("Altitude calibrated", TOAST_SUCCESS);
            if (alt_label) {
                float altitude = 44330.0f * (1.0f - powf(last_press / sea_level_pressure, 0.1903f));
                char buf[64];
                snprintf(buf, sizeof(buf), "%.0f m / %.0f ft",
                         (double)altitude, (double)(altitude * 3.28084f));
                lv_label_set_text(alt_label, buf);
            }
        } else {
            toast_show("No pressure data", TOAST_WARN);
        }
    } else if (event->type == INPUT_TYPE_JOYSTICK || event->type == INPUT_TYPE_EXIT_BUTTON) {
        display_manager_switch_view(&main_menu_view);
    }
}

static void get_enviii_callback(void **callback) {
    if (callback) *callback = (void *)enviii_event_handler;
}

/* -------------------------------------------------------------------------- */
/* Helpers                                                                    */
/* -------------------------------------------------------------------------- */
static lv_obj_t *make_card(lv_obj_t *parent, int y, int w, int h) {
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, w, h);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_radius(card, 10, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, 4, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, y);
    return card;
}

static lv_obj_t *make_icon(lv_obj_t *parent, const char *txt, uint32_t color) {
    lv_obj_t *icon = lv_label_create(parent);
    lv_label_set_text(icon, txt);
    lv_obj_set_style_text_color(icon, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(icon, accessibility_get_font_title(), 0);
    return icon;
}

/* -------------------------------------------------------------------------- */
/* Create / Destroy                                                           */
/* -------------------------------------------------------------------------- */
void enviii_create(void) {
    display_manager_fill_screen(lv_color_hex(0x000000));
    enviii_container = gui_screen_create_root(NULL, "ENV-III", lv_color_hex(0x000000), LV_OPA_COVER);
    enviii_view.root = enviii_container;

    lv_obj_t *content = gui_screen_create_content(enviii_container, GUI_STATUS_BAR_HEIGHT);
    lv_obj_set_style_text_color(content, lv_color_hex(0xFFFFFF), 0);

    /* Title */
    lv_obj_t *title = lv_label_create(content);
    lv_label_set_text(title, "ENV-III");
    lv_obj_set_style_text_color(title, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(title, accessibility_get_font_body(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 2);

    /* Temperature card (large, full width) */
    lv_obj_t *temp_card = make_card(content, 28, 216, 56);
    temp_label = lv_label_create(temp_card);
    lv_label_set_text(temp_label, "--.- C / --.- F");
    lv_obj_set_style_text_color(temp_label, lv_color_hex(0xFF9800), 0);
    lv_obj_set_style_text_font(temp_label, accessibility_get_font_title(), 0);
    lv_obj_center(temp_label);

    /* Humidity card */
    lv_obj_t *hum_card = make_card(content, 92, 216, 44);
    lv_obj_set_flex_flow(hum_card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hum_card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(hum_card, 10, 0);
    make_icon(hum_card, "~", 0x00BCD4);
    hum_label = lv_label_create(hum_card);
    lv_label_set_text(hum_label, "--.- %%");
    lv_obj_set_style_text_color(hum_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(hum_label, accessibility_get_font_body(), 0);

    /* Pressure card */
    lv_obj_t *press_card = make_card(content, 144, 216, 44);
    lv_obj_set_flex_flow(press_card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(press_card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(press_card, 10, 0);
    make_icon(press_card, "#", 0x8BC34A);
    press_label = lv_label_create(press_card);
    lv_label_set_text(press_label, "---.- hPa / --.-- inHg");
    lv_obj_set_style_text_color(press_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(press_label, accessibility_get_font_body(), 0);

    /* Altitude card */
    lv_obj_t *alt_card = make_card(content, 196, 216, 44);
    lv_obj_set_flex_flow(alt_card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(alt_card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(alt_card, 10, 0);
    make_icon(alt_card, "^", 0xFF5722);
    alt_label = lv_label_create(alt_card);
    lv_label_set_text(alt_label, "--- m / --- ft");
    lv_obj_set_style_text_color(alt_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(alt_label, accessibility_get_font_body(), 0);

    /* Calibration hint */
    lv_obj_t *hint = lv_label_create(content);
    lv_label_set_text(hint, "Touch to set current altitude as 0");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x666666), 0);
    lv_obj_set_style_text_font(hint, accessibility_get_font_small(), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -26);

    /* Status */
    status_label = lv_label_create(content);
    lv_label_set_text(status_label, "Initializing...");
    lv_obj_set_style_text_color(status_label, lv_color_hex(0xAAAAAA), 0);
    lv_obj_set_style_text_font(status_label, accessibility_get_font_small(), 0);
    lv_obj_align(status_label, LV_ALIGN_BOTTOM_MID, 0, -4);

    sensors_ready_shown = false;
    sht_data_valid = false;
    qmp_data_valid = false;
    sea_level_pressure = 1013.25f;
    last_temp = 0.0f;
    last_hum = 0.0f;
    last_press = 0.0f;

    sht30_init_hw();
    qmp6988_init_hw();

    enviii_timer = lv_timer_create(enviii_timer_cb, 500, NULL);
    /* immediate first sample */
    enviii_timer_cb(enviii_timer);
}

void enviii_destroy(void) {
    if (enviii_timer) {
        lv_timer_del(enviii_timer);
        enviii_timer = NULL;
    }
    if (s_sht30_dev) {
        i2c_master_bus_rm_device(s_sht30_dev);
        s_sht30_dev = NULL;
    }
    if (s_qmp6988_dev) {
        i2c_master_bus_rm_device(s_qmp6988_dev);
        s_qmp6988_dev = NULL;
    }
    if (enviii_container) {
        lv_obj_del(enviii_container);
        enviii_container = NULL;
        enviii_view.root = NULL;
    }
    temp_label = NULL;
    hum_label = NULL;
    press_label = NULL;
    alt_label = NULL;
    status_label = NULL;
    sht30_initialized = false;
    qmp6988_initialized = false;
    sensors_ready_shown = false;
    sht_data_valid = false;
    qmp_data_valid = false;
}

View enviii_view = {
    .root = NULL,
    .create = enviii_create,
    .destroy = enviii_destroy,
    .input_callback = enviii_event_handler,
    .name = "ENV-III",
    .get_hardwareinput_callback = get_enviii_callback
};

#endif /* CONFIG_HAS_ENVIII */
