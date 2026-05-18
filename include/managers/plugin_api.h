#ifndef PLUGIN_API_H
#define PLUGIN_API_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GHOSTESP_APP_API_VERSION 1u
#define GHOSTESP_APP_FLAG_PERMISSIONS_ENFORCED (1u << 0)
#define GHOSTESP_APP_FLAG_ABSOLUTE_STORAGE_ALLOWED (1u << 1)

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

typedef struct {
    uint8_t bssid[6];
    char ssid[33];
    uint8_t channel;
    int8_t rssi;
    uint8_t auth_mode;
} ghostesp_wifi_ap_info_t;

#define GHOSTESP_STORAGE_NAME_MAX 64

typedef struct {
    char name[GHOSTESP_STORAGE_NAME_MAX];
    bool is_directory;
} ghostesp_storage_entry_t;

typedef struct {
    uint8_t mac[6];
    int8_t rssi;
    char name[32];
} ghostesp_ble_device_info_t;

typedef struct {
    uint8_t type;
    uint8_t mac[6];
    int8_t rssi;
    char name[32];
    char subtype[20];
    bool tracking;
} ghostesp_ble_detect_info_t;

typedef void *ghostesp_ui_obj_t;
typedef void (*ghostesp_ui_button_cb_t)(void *user);

typedef void *ghostesp_options_t;
typedef void *ghostesp_detail_t;
typedef void *ghostesp_popup_t;
typedef void *ghostesp_scan_t;
typedef void *ghostesp_ui_timer_t;
typedef void *ghostesp_paged_menu_t;

typedef enum {
    GHOSTESP_FONT_MICRO = 0,
    GHOSTESP_FONT_CAPTION,
    GHOSTESP_FONT_BODY,
    GHOSTESP_FONT_TITLE,
} ghostesp_font_size_t;

typedef enum {
    GHOSTESP_ALIGN_CENTER = 0,
    GHOSTESP_ALIGN_TOP_LEFT,
    GHOSTESP_ALIGN_TOP_RIGHT,
    GHOSTESP_ALIGN_BOTTOM_LEFT,
    GHOSTESP_ALIGN_BOTTOM_RIGHT,
    GHOSTESP_ALIGN_TOP_MID,
    GHOSTESP_ALIGN_BOTTOM_MID,
    GHOSTESP_ALIGN_LEFT_MID,
    GHOSTESP_ALIGN_RIGHT_MID,
    GHOSTESP_ALIGN_OUT_TOP_LEFT,
    GHOSTESP_ALIGN_OUT_TOP_MID,
    GHOSTESP_ALIGN_OUT_TOP_RIGHT,
    GHOSTESP_ALIGN_OUT_BOTTOM_LEFT,
    GHOSTESP_ALIGN_OUT_BOTTOM_MID,
    GHOSTESP_ALIGN_OUT_BOTTOM_RIGHT,
    GHOSTESP_ALIGN_OUT_LEFT_TOP,
    GHOSTESP_ALIGN_OUT_LEFT_MID,
    GHOSTESP_ALIGN_OUT_LEFT_BOTTOM,
    GHOSTESP_ALIGN_OUT_RIGHT_TOP,
    GHOSTESP_ALIGN_OUT_RIGHT_MID,
    GHOSTESP_ALIGN_OUT_RIGHT_BOTTOM,
} ghostesp_align_t;

typedef enum {
    GHOSTESP_FLEX_FLOW_NONE = 0,
    GHOSTESP_FLEX_FLOW_COLUMN,
    GHOSTESP_FLEX_FLOW_ROW,
    GHOSTESP_FLEX_FLOW_COLUMN_WRAP,
    GHOSTESP_FLEX_FLOW_ROW_WRAP,
    GHOSTESP_FLEX_FLOW_COLUMN_REVERSE,
    GHOSTESP_FLEX_FLOW_ROW_REVERSE,
    GHOSTESP_FLEX_FLOW_COLUMN_WRAP_REVERSE,
    GHOSTESP_FLEX_FLOW_ROW_WRAP_REVERSE,
} ghostesp_flex_flow_t;

typedef enum {
    GHOSTESP_FLEX_ALIGN_START = 0,
    GHOSTESP_FLEX_ALIGN_END,
    GHOSTESP_FLEX_ALIGN_CENTER,
    GHOSTESP_FLEX_ALIGN_SPACE_EVENLY,
    GHOSTESP_FLEX_ALIGN_SPACE_AROUND,
    GHOSTESP_FLEX_ALIGN_SPACE_BETWEEN,
} ghostesp_flex_align_t;

typedef struct {
    int32_t x;
    int32_t y;
} ghostesp_point_t;

typedef void (*ghostesp_ui_timer_cb_t)(void *user);
typedef void (*ghostesp_paged_menu_load_fn)(int offset, int page_size, char names[][128], bool *has_more, void *user);
typedef void (*ghostesp_paged_menu_select_fn)(const char *name, void *user);
typedef void (*ghostesp_paged_menu_nav_fn)(void *user);
typedef void (*ghostesp_anim_done_cb_t)(void *user);
typedef void (*ghostesp_input_submit_cb_t)(const char *text, void *user);

typedef struct ghostesp_api {
    uint32_t api_version;
    size_t struct_size;
    uint32_t flags;
    const char *target;

    void (*log)(const char *message);
    void (*ui_set_title)(const char *title);
    void (*ui_print)(const char *text);
    void (*ui_clear)(void);
    void (*toast)(const char *message);
    void (*ui_show_text)(const char *title, const char *text);

    bool (*command_exec)(const char *command);

    bool (*storage_exists)(const char *path);
    int (*storage_read)(const char *path, void *buffer, size_t buffer_len);
    bool (*storage_write)(const char *path, const void *data, size_t len);
    bool (*storage_append)(const char *path, const void *data, size_t len);
    bool (*storage_delete)(const char *path);
    bool (*storage_mkdir)(const char *path);
    int (*storage_list)(const char *path, ghostesp_storage_entry_t *out, int max_entries);

    void *(*malloc)(size_t size);
    void (*free)(void *ptr);
    void *(*app_malloc)(size_t size);
    void *(*app_calloc)(size_t count, size_t size);
    void (*app_free)(void *ptr);
    size_t (*app_memory_used)(void);
    size_t (*app_memory_limit)(void);
    void (*delay_ms)(uint32_t ms);

    size_t (*system_free_heap)(void);
    size_t (*system_free_internal_heap)(void);
    uint32_t (*system_uptime_ms)(void);
    const char *(*system_firmware_version)(void);
    const char *(*system_target)(void);

    bool (*wifi_start_scan)(void);
    bool (*wifi_stop_scan)(void);
    uint16_t (*wifi_ap_count)(void);
    bool (*wifi_scan_get_ap)(uint16_t index, ghostesp_wifi_ap_info_t *out);

    bool (*rgb_set_all)(uint8_t red, uint8_t green, uint8_t blue);

    void *(*lv_scr_act)(void);
    void *(*display_get_current_view)(void);
    void *(*raw_symbol)(const char *name);

    const char *(*app_id)(void);
    const char *(*app_data_path)(void);
    bool (*app_storage_exists)(const char *path);
    int (*app_storage_read)(const char *path, void *buffer, size_t buffer_len);
    bool (*app_storage_write)(const char *path, const void *data, size_t len);
    bool (*app_storage_append)(const char *path, const void *data, size_t len);
    bool (*app_storage_delete)(const char *path);
    bool (*app_storage_mkdir)(const char *path);
    int (*app_storage_list)(const char *path, ghostesp_storage_entry_t *out, int max_entries);

    bool (*badusb_run_script)(const char *app_relative_path);
    bool (*badusb_stop)(void);

    bool (*ir_send_file)(const char *app_relative_path);
    bool (*ir_stop)(void);

    bool (*nfc_is_available)(void);
    bool (*nfc_read_start)(void);
    bool (*nfc_stop)(void);

    bool (*ble_start_scan)(void);
    bool (*ble_stop_scan)(void);
    int (*ble_device_count)(void);
    bool (*ble_get_device)(int index, ghostesp_ble_device_info_t *out);

    bool (*subghz_is_available)(void);
    bool (*subghz_load_snapshot)(const char *app_relative_path);
    bool (*subghz_transmit_loaded)(void);
    bool (*subghz_stop)(void);

    ghostesp_ui_obj_t (*ui_screen_create)(const char *title);
    ghostesp_ui_obj_t (*ui_card_create)(ghostesp_ui_obj_t parent);
    ghostesp_ui_obj_t (*ui_label_create)(ghostesp_ui_obj_t parent, const char *text);
    ghostesp_ui_obj_t (*ui_button_create)(ghostesp_ui_obj_t parent, const char *text, ghostesp_ui_button_cb_t on_click, void *user);
    void (*ui_label_set_text)(ghostesp_ui_obj_t label, const char *text);
    void (*ui_button_set_text)(ghostesp_ui_obj_t button, const char *text);
    void (*ui_obj_set_visible)(ghostesp_ui_obj_t obj, bool visible);
    void (*ui_obj_delete)(ghostesp_ui_obj_t obj);
    void (*ui_set_status)(const char *text);
    void (*ui_show_popup)(const char *title, const char *text);

    uint32_t (*ui_theme_get_background)(void);
    uint32_t (*ui_theme_get_surface)(void);
    uint32_t (*ui_theme_get_surface_alt)(void);
    uint32_t (*ui_theme_get_text)(void);
    uint32_t (*ui_theme_get_text_muted)(void);
    uint32_t (*ui_theme_get_accent)(void);
    bool (*ui_theme_is_bright)(void);

    void (*ui_obj_set_bg_color)(ghostesp_ui_obj_t obj, uint32_t hex_color);
    void (*ui_obj_set_text_color)(ghostesp_ui_obj_t obj, uint32_t hex_color);
    void (*ui_obj_set_border_color)(ghostesp_ui_obj_t obj, uint32_t hex_color);
    void (*ui_obj_set_border_width)(ghostesp_ui_obj_t obj, int32_t width);
    void (*ui_obj_set_radius)(ghostesp_ui_obj_t obj, int32_t radius);
    void (*ui_obj_set_pad)(ghostesp_ui_obj_t obj, int32_t left, int32_t right, int32_t top, int32_t bottom);
    void (*ui_obj_set_font)(ghostesp_ui_obj_t obj, ghostesp_font_size_t size);
    void (*ui_obj_set_opa)(ghostesp_ui_obj_t obj, uint8_t opa);

    void (*ui_obj_set_pos)(ghostesp_ui_obj_t obj, int32_t x, int32_t y);
    void (*ui_obj_set_size)(ghostesp_ui_obj_t obj, int32_t w, int32_t h);
    void (*ui_obj_set_width)(ghostesp_ui_obj_t obj, int32_t w);
    void (*ui_obj_set_height)(ghostesp_ui_obj_t obj, int32_t h);
    void (*ui_obj_align)(ghostesp_ui_obj_t obj, ghostesp_align_t align, int32_t x_ofs, int32_t y_ofs);
    int32_t (*ui_obj_get_width)(ghostesp_ui_obj_t obj);
    int32_t (*ui_obj_get_height)(ghostesp_ui_obj_t obj);
    int32_t (*ui_obj_get_x)(ghostesp_ui_obj_t obj);
    int32_t (*ui_obj_get_y)(ghostesp_ui_obj_t obj);

    void (*ui_obj_set_flex_flow)(ghostesp_ui_obj_t obj, ghostesp_flex_flow_t flow);
    void (*ui_obj_set_flex_align)(ghostesp_ui_obj_t obj, ghostesp_flex_align_t main, ghostesp_flex_align_t cross, ghostesp_flex_align_t track);
    void (*ui_obj_set_flex_grow)(ghostesp_ui_obj_t obj, uint8_t grow);
    void (*ui_obj_set_pad_row)(ghostesp_ui_obj_t obj, int32_t pad);
    void (*ui_obj_set_pad_column)(ghostesp_ui_obj_t obj, int32_t pad);

    ghostesp_ui_timer_t (*ui_timer_create)(ghostesp_ui_timer_cb_t cb, uint32_t interval_ms, void *user);
    void (*ui_timer_delete)(ghostesp_ui_timer_t timer);
    void (*ui_timer_set_interval)(ghostesp_ui_timer_t timer, uint32_t interval_ms);

    ghostesp_options_t (*ui_options_create)(const char *title);
    ghostesp_ui_obj_t (*ui_options_add_item)(ghostesp_options_t opts, const char *label, ghostesp_ui_button_cb_t on_click, void *user);
    ghostesp_ui_obj_t (*ui_options_add_back)(ghostesp_options_t opts, ghostesp_ui_button_cb_t on_click, void *user);
    void (*ui_options_set_selected)(ghostesp_options_t opts, int index);
    void (*ui_options_move_selection)(ghostesp_options_t opts, int delta);
    int (*ui_options_get_selected)(ghostesp_options_t opts);
    void (*ui_options_clear)(ghostesp_options_t opts);
    void (*ui_options_destroy)(ghostesp_options_t opts);

    ghostesp_detail_t (*ui_detail_create)(const char *title);
    void (*ui_detail_add_info)(ghostesp_detail_t dv, const char *label, const char *value);
    void (*ui_detail_add_action)(ghostesp_detail_t dv, const char *label, ghostesp_ui_button_cb_t on_click, void *user);
    void (*ui_detail_add_header)(ghostesp_detail_t dv, const char *text);
    void (*ui_detail_add_divider)(ghostesp_detail_t dv);
    ghostesp_ui_obj_t (*ui_detail_add_back)(ghostesp_detail_t dv, ghostesp_ui_button_cb_t on_click, void *user);
    void (*ui_detail_set_selected)(ghostesp_detail_t dv, int index);
    void (*ui_detail_move_selection)(ghostesp_detail_t dv, int delta);
    int (*ui_detail_get_selected)(ghostesp_detail_t dv);
    int (*ui_detail_get_count)(ghostesp_detail_t dv);
    void (*ui_detail_clear)(ghostesp_detail_t dv);
    void (*ui_detail_destroy)(ghostesp_detail_t dv);

    ghostesp_popup_t (*ui_popup_create)(int32_t width, int32_t height);
    void (*ui_popup_set_title)(ghostesp_popup_t p, const char *title);
    void (*ui_popup_set_body)(ghostesp_popup_t p, const char *body);
    ghostesp_ui_obj_t (*ui_popup_add_button)(ghostesp_popup_t p, const char *label, ghostesp_ui_button_cb_t on_click, void *user);
    void (*ui_popup_show)(ghostesp_popup_t p);
    void (*ui_popup_hide)(ghostesp_popup_t p);
    void (*ui_popup_destroy)(ghostesp_popup_t p);

    ghostesp_scan_t (*ui_scan_status_create)(const char *message);
    void (*ui_scan_status_update)(ghostesp_scan_t ss, const char *message);
    void (*ui_scan_status_set_progress)(ghostesp_scan_t ss, int current, int total);
    void (*ui_scan_status_close)(ghostesp_scan_t ss);

    ghostesp_ui_obj_t (*ui_canvas_create)(ghostesp_ui_obj_t parent, int32_t width, int32_t height);
    void (*ui_canvas_draw_rect)(ghostesp_ui_obj_t canvas, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t hex_color);
    void (*ui_canvas_fill)(ghostesp_ui_obj_t canvas, uint32_t hex_color);
    void (*ui_canvas_draw_line)(ghostesp_ui_obj_t canvas, const ghostesp_point_t *points, int count, uint32_t hex_color, int32_t width);
    void (*ui_canvas_draw_arc)(ghostesp_ui_obj_t canvas, int32_t cx, int32_t cy, int32_t r, int32_t start_angle, int32_t end_angle, uint32_t hex_color, int32_t width);

    void (*ui_anim_slide_in)(ghostesp_ui_obj_t obj, int direction, uint32_t duration_ms);
    void (*ui_anim_slide_out)(ghostesp_ui_obj_t obj, int direction, uint32_t duration_ms, ghostesp_anim_done_cb_t on_done, void *user);
    void (*ui_anim_pop_in)(ghostesp_ui_obj_t obj);
    void (*ui_anim_press_pulse)(ghostesp_ui_obj_t obj);

    ghostesp_ui_obj_t (*ui_arc_create)(ghostesp_ui_obj_t parent);
    void (*ui_arc_set_value)(ghostesp_ui_obj_t arc, int32_t value);
    void (*ui_arc_set_range)(ghostesp_ui_obj_t arc, int32_t min, int32_t max);
    void (*ui_arc_set_angles)(ghostesp_ui_obj_t arc, int32_t start, int32_t end);
    void (*ui_arc_set_bg_angles)(ghostesp_ui_obj_t arc, int32_t start, int32_t end);
    void (*ui_arc_set_bg_color)(ghostesp_ui_obj_t arc, uint32_t hex_color);
    void (*ui_arc_set_indicator_color)(ghostesp_ui_obj_t arc, uint32_t hex_color);

    ghostesp_ui_obj_t (*ui_line_create)(ghostesp_ui_obj_t parent);
    void (*ui_line_set_points)(ghostesp_ui_obj_t line, const ghostesp_point_t *points, int count);
    void (*ui_line_set_color)(ghostesp_ui_obj_t line, uint32_t hex_color);
    void (*ui_line_set_width)(ghostesp_ui_obj_t line, int32_t width);

    ghostesp_ui_obj_t (*ui_image_create)(ghostesp_ui_obj_t parent);
    bool (*ui_image_set_src)(ghostesp_ui_obj_t img, const char *app_relative_path);

    ghostesp_paged_menu_t (*ui_paged_menu_create)(int page_size, ghostesp_paged_menu_load_fn load_fn, void *user);
    void (*ui_paged_menu_set_callbacks)(ghostesp_paged_menu_t pm, ghostesp_paged_menu_select_fn select_fn, ghostesp_paged_menu_nav_fn prev_fn, ghostesp_paged_menu_nav_fn next_fn, void *user);
    void (*ui_paged_menu_reset)(ghostesp_paged_menu_t pm);
    void (*ui_paged_menu_destroy)(ghostesp_paged_menu_t pm);
    bool (*ui_paged_menu_has_prev)(ghostesp_paged_menu_t pm);
    bool (*ui_paged_menu_has_next)(ghostesp_paged_menu_t pm);

    bool (*gps_is_available)(void);
    bool (*gps_has_fix)(void);
    double (*gps_get_latitude)(void);
    double (*gps_get_longitude)(void);
    double (*gps_get_altitude)(void);
    int (*gps_get_satellites)(void);
    float (*gps_get_speed)(void);
    float (*gps_get_heading)(void);

    uint8_t (*settings_get_theme)(void);
    const char *(*settings_get_device_name)(void);

    void (*ui_input_dialog)(const char *title, const char *default_text, ghostesp_input_submit_cb_t on_submit, void *user);

    int32_t (*ui_screen_get_width)(void);
    int32_t (*ui_screen_get_height)(void);

    void (*ble_detect_start)(void);
    void (*ble_detect_stop)(void);
    bool (*ble_detect_is_active)(void);
    int (*ble_detect_count)(void);
    bool (*ble_detect_get_device)(int index, ghostesp_ble_detect_info_t *out);
    const char *(*ble_detect_type_name)(uint8_t type);
    bool (*ble_detect_start_tracking)(int index);
    bool (*ble_detect_start_airtag_spoof)(int index);
    bool (*ui_detail_step_up)(ghostesp_detail_t dv);
    bool (*ui_detail_step_down)(ghostesp_detail_t dv);
    void (*ui_detail_activate_selected)(ghostesp_detail_t dv);
    void (*app_exit)(void);
} ghostesp_api_t;

#define GHOSTESP_API_STRUCT_SIZE_V1 sizeof(ghostesp_api_t)

typedef struct ghostesp_app {
    uint32_t api_version;
    size_t struct_size;
    const char *id;
    const char *name;
    void (*on_start)(void);
    void (*on_stop)(void);
    void (*on_input)(const ghostesp_input_event_t *event);
    void (*on_tick)(uint32_t elapsed_ms);
    void (*on_pause)(void);
    void (*on_resume)(void);
} ghostesp_app_t;

#define GHOSTESP_APP_STRUCT_SIZE_V1 sizeof(ghostesp_app_t)

typedef const ghostesp_app_t *(*ghostesp_app_init_fn)(const ghostesp_api_t *api);

const ghostesp_api_t *plugin_api_get(const char *app_id,
                                     uint32_t permissions,
                                     size_t memory_limit,
                                     bool allow_absolute_storage);
const char *plugin_api_current_target(void);
bool plugin_api_is_active(void);
void plugin_api_release(void);
void plugin_api_set_ui_hooks(void (*set_title)(const char *title),
                             void (*print)(const char *text),
                             void (*clear)(void),
                             void (*toast)(const char *message));

#ifdef __cplusplus
}
#endif

#endif
