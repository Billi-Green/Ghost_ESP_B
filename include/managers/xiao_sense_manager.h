#ifndef XIAO_SENSE_MANAGER_H
#define XIAO_SENSE_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    bool available;
    int voltage_mv;
    int percentage;
    bool charging;
} xiao_sense_battery_t;

typedef struct {
    bool enabled;
    uint8_t brightness;
    bool sd_active;
} xiao_sense_led_t;

bool xiao_sense_manager_is_supported(void);
esp_err_t xiao_sense_manager_init(void);
bool xiao_sense_manager_get_battery(xiao_sense_battery_t *out);
void xiao_sense_manager_get_led(xiao_sense_led_t *out);
esp_err_t xiao_sense_manager_set_led(bool enabled, uint8_t brightness);

/* GPIO7 is released while the shared SPI SD card is mounted. */
void xiao_sense_manager_sd_mount_begin(void);
void xiao_sense_manager_sd_mount_end(void);

#endif
