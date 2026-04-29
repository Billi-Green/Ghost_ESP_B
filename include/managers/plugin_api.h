#ifndef PLUGIN_API_H
#define PLUGIN_API_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GHOSTESP_APP_API_VERSION 1u
#define GHOSTESP_APP_FLAG_UNSAFE_ALLOWED (1u << 0)

typedef enum {
    GHOSTESP_INPUT_NONE = 0,
    GHOSTESP_INPUT_LEFT,
    GHOSTESP_INPUT_RIGHT,
    GHOSTESP_INPUT_UP,
    GHOSTESP_INPUT_DOWN,
    GHOSTESP_INPUT_SELECT,
    GHOSTESP_INPUT_BACK,
    GHOSTESP_INPUT_KEY,
    GHOSTESP_INPUT_TOUCH,
} ghostesp_input_type_t;

typedef struct {
    ghostesp_input_type_t type;
    int32_t value;
    int32_t x;
    int32_t y;
    bool pressed;
} ghostesp_input_event_t;

typedef struct ghostesp_unsafe_api {
    uint32_t api_version;
    void *(*lv_scr_act)(void);
    void *(*display_get_current_view)(void);
    void *(*raw_symbol)(const char *name);
} ghostesp_unsafe_api_t;

typedef struct ghostesp_api {
    uint32_t api_version;
    uint32_t flags;
    const char *target;

    void (*log)(const char *message);
    void (*ui_set_title)(const char *title);
    void (*ui_print)(const char *text);
    void (*ui_clear)(void);
    void (*toast)(const char *message);

    bool (*command_exec)(const char *command);

    bool (*storage_exists)(const char *path);
    int (*storage_read)(const char *path, void *buffer, size_t buffer_len);
    bool (*storage_write)(const char *path, const void *data, size_t len);

    void *(*malloc)(size_t size);
    void (*free)(void *ptr);
    void (*delay_ms)(uint32_t ms);

    size_t (*system_free_heap)(void);
    size_t (*system_free_internal_heap)(void);
    uint32_t (*system_uptime_ms)(void);

    bool (*wifi_start_scan)(void);
    bool (*wifi_stop_scan)(void);
    uint16_t (*wifi_ap_count)(void);

    bool (*rgb_set_all)(uint8_t red, uint8_t green, uint8_t blue);

    const ghostesp_unsafe_api_t *unsafe;
} ghostesp_api_t;

typedef struct ghostesp_app {
    uint32_t api_version;
    const char *id;
    const char *name;
    void (*on_start)(void);
    void (*on_stop)(void);
    void (*on_input)(const ghostesp_input_event_t *event);
    void (*on_tick)(uint32_t elapsed_ms);
} ghostesp_app_t;

typedef const ghostesp_app_t *(*ghostesp_app_init_fn)(const ghostesp_api_t *api);

const ghostesp_api_t *plugin_api_get(bool unsafe_allowed);
const char *plugin_api_current_target(void);
void plugin_api_set_ui_hooks(void (*set_title)(const char *title),
                             void (*print)(const char *text),
                             void (*clear)(void),
                             void (*toast)(const char *message));

#ifdef __cplusplus
}
#endif

#endif
