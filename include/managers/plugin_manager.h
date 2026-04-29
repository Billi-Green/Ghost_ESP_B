#ifndef PLUGIN_MANAGER_H
#define PLUGIN_MANAGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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
} plugin_permission_t;

typedef struct {
    char id[PLUGIN_APP_ID_MAX];
    char name[PLUGIN_APP_NAME_MAX];
    char version[PLUGIN_APP_VERSION_MAX];
    char author[PLUGIN_APP_AUTHOR_MAX];
    char target[PLUGIN_APP_TARGET_MAX];
    char entry[PLUGIN_APP_ENTRY_MAX];
    char base_path[PLUGIN_APP_PATH_MAX];
    char entry_path[PLUGIN_APP_PATH_MAX];
    uint32_t api_version;
    uint32_t permissions;
    bool unsafe;
    bool valid;
    char error[96];
} plugin_app_manifest_t;

void plugin_manager_init(void);
int plugin_manager_reload(void);
int plugin_manager_count(void);
const plugin_app_manifest_t *plugin_manager_get(int index);
const plugin_app_manifest_t *plugin_manager_find(const char *id);
bool plugin_manager_target_supported(void);
bool plugin_manager_target_matches(const plugin_app_manifest_t *app);
const char *plugin_manager_last_error(void);

#ifdef __cplusplus
}
#endif

#endif
