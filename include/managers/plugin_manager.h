#ifndef PLUGIN_MANAGER_H
#define PLUGIN_MANAGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PLUGIN_APP_MAX_COUNT 32
#define PLUGIN_APP_ID_MAX 32
#define PLUGIN_APP_NAME_MAX 64
#define PLUGIN_APP_VERSION_MAX 24
#define PLUGIN_APP_AUTHOR_MAX 64
#define PLUGIN_APP_TARGET_MAX 16
#define PLUGIN_APP_ENTRY_MAX 64
#define PLUGIN_APP_PATH_MAX 384
#define PLUGIN_APP_DESC_MAX 160
#define PLUGIN_APP_CATEGORY_MAX 32
#define PLUGIN_APP_ICON_MAX 64
#define PLUGIN_APP_STORAGE_SCOPE_MAX 16
#define PLUGIN_APP_ACCENT_COLOR_MAX 8
#define PLUGIN_APP_CHECKSUM_MAX 65

#define PLUGIN_APP_MANIFEST_VERSION 1u
#define PLUGIN_APP_DATA_VERSION_DEFAULT 1u
#define PLUGIN_APP_STORAGE_SCOPE_APP "app"
#define PLUGIN_APP_STORAGE_SCOPE_GHOSTESP "ghostesp"
#define PLUGIN_APP_QUARANTINE_THRESHOLD 3u

typedef enum {
    PLUGIN_PERMISSION_UI       = 1u << 0,
    PLUGIN_PERMISSION_STORAGE  = 1u << 1,
    PLUGIN_PERMISSION_COMMANDS = 1u << 2,
    PLUGIN_PERMISSION_TASKS    = 1u << 3,
    PLUGIN_PERMISSION_WIFI     = 1u << 4,
    PLUGIN_PERMISSION_BLE      = 1u << 5,
    PLUGIN_PERMISSION_NFC      = 1u << 6,
    PLUGIN_PERMISSION_IR       = 1u << 7,
    PLUGIN_PERMISSION_SUBGHZ   = 1u << 8,
    PLUGIN_PERMISSION_BADUSB   = 1u << 9,
    PLUGIN_PERMISSION_RAW_GPIO = 1u << 10,
    PLUGIN_PERMISSION_LVGL     = 1u << 11,
    PLUGIN_PERMISSION_RGB      = 1u << 12,
    PLUGIN_PERMISSION_UART     = 1u << 13,
    PLUGIN_PERMISSION_I2C      = 1u << 14,
    PLUGIN_PERMISSION_SPI      = 1u << 15,
    PLUGIN_PERMISSION_ADC      = 1u << 16,
    PLUGIN_PERMISSION_PWM      = 1u << 17,
    PLUGIN_PERMISSION_NETWORK  = 1u << 18,
    PLUGIN_PERMISSION_WIFI_CONTROL = 1u << 19,
    PLUGIN_PERMISSION_POWER    = 1u << 20,
    PLUGIN_PERMISSION_INPUT    = 1u << 21,
    PLUGIN_PERMISSION_DISPLAY  = 1u << 22,
    PLUGIN_PERMISSION_TIME     = 1u << 23,
    PLUGIN_PERMISSION_RANDOM   = 1u << 24,
    PLUGIN_PERMISSION_SYSTEM   = 1u << 25,
    PLUGIN_PERMISSION_CAMERA   = 1u << 26,
    PLUGIN_PERMISSION_USB      = 1u << 27,
    PLUGIN_PERMISSION_ETHERNET = 1u << 28,
    PLUGIN_PERMISSION_AUDIO    = 1u << 29,
    PLUGIN_PERMISSION_SETTINGS = 1u << 30,
    PLUGIN_PERMISSION_ZIGBEE   = 1u << 31,
} plugin_permission_t;

typedef struct {
    char id[PLUGIN_APP_ID_MAX];
    char name[PLUGIN_APP_NAME_MAX];
    char version[PLUGIN_APP_VERSION_MAX];
    char author[PLUGIN_APP_AUTHOR_MAX];
    char target[PLUGIN_APP_TARGET_MAX];
    char entry[PLUGIN_APP_ENTRY_MAX];
    char description[PLUGIN_APP_DESC_MAX];
    char category[PLUGIN_APP_CATEGORY_MAX];
    char icon[PLUGIN_APP_ICON_MAX];
    char icon_format[PLUGIN_APP_CATEGORY_MAX];
    char accent_color[PLUGIN_APP_ACCENT_COLOR_MAX];
    char storage_scope[PLUGIN_APP_STORAGE_SCOPE_MAX];
    char firmware_min[PLUGIN_APP_VERSION_MAX];
    char firmware_max[PLUGIN_APP_VERSION_MAX];
    char checksum[PLUGIN_APP_CHECKSUM_MAX];
    char base_path[PLUGIN_APP_PATH_MAX];
    char entry_path[PLUGIN_APP_PATH_MAX];
    uint32_t api_version;
    uint32_t manifest_version;
    uint32_t package_version;
    uint32_t data_version;
    uint32_t memory_limit;
    uint32_t stack_size;
    uint16_t icon_width;
    uint16_t icon_height;
    uint32_t launch_failure_count;
    uint32_t permissions;
    bool requires_psram;
    bool allow_absolute_storage;
    bool quarantined;
    bool valid;
    const lv_img_dsc_t *icon_dsc;
    char error[96];
} plugin_app_manifest_t;

void plugin_manager_init(void);
int plugin_manager_reload(void);
int plugin_manager_count(void);
const plugin_app_manifest_t *plugin_manager_get(int index);
const plugin_app_manifest_t *plugin_manager_find(const char *id);
const lv_img_dsc_t *plugin_manager_get_icon(const plugin_app_manifest_t *app);
bool plugin_manager_target_supported(void);
bool plugin_manager_target_matches(const plugin_app_manifest_t *app);
bool plugin_manager_reset_app_state(const char *id);
const char *plugin_manager_last_error(void);

#ifdef __cplusplus
}
#endif

#endif
