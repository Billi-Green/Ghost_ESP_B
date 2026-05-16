#include "managers/plugin_api.h"

#include "core/serial_manager.h"
#include "core/glog.h"
#include "gui/design_tokens.h"
#include "gui/screen_layout.h"
#include "managers/badusb_manager.h"
#include "managers/ble_manager.h"
#include "managers/display_manager.h"
#include "managers/infrared_manager.h"
#include "managers/plugin_manager.h"
#include "managers/rgb_manager.h"
#include "managers/sd_card_manager.h"
#include "managers/subghz_remote_manager.h"
#include "managers/wifi_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_app_desc.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#if CONFIG_ENABLE_NATIVE_SD_APPS
#include "esp_elf.h"
#endif

static const char *TAG = "PluginAPI";

static void (*s_ui_set_title)(const char *title) = NULL;
static void (*s_ui_print)(const char *text) = NULL;
static void (*s_ui_clear)(void) = NULL;
static void (*s_ui_toast)(const char *message) = NULL;
static char s_app_id[PLUGIN_APP_ID_MAX];
static char s_app_data_path[PLUGIN_APP_PATH_MAX];
static uint32_t s_permissions = 0;
static bool s_allow_absolute_storage = false;
static size_t s_memory_limit = 0;
static size_t s_memory_used = 0;
static bool s_api_active = false;
static SemaphoreHandle_t s_api_mutex = NULL;

#define PLUGIN_ALLOC_MAGIC 0x47415050u

typedef struct {
    uint32_t magic;
    size_t size;
} plugin_alloc_header_t;

typedef struct {
    void (*fn)(void *ctx);
    void *ctx;
    SemaphoreHandle_t done;
} plugin_ui_sync_call_t;

typedef struct {
    ghostesp_ui_button_cb_t cb;
    void *user;
} plugin_ui_button_ctx_t;

typedef struct {
    const char *title;
    const char *text;
    ghostesp_ui_button_cb_t cb;
    void *user;
    ghostesp_ui_obj_t parent;
    ghostesp_ui_obj_t result;
} plugin_ui_create_ctx_t;

typedef struct {
    ghostesp_ui_obj_t obj;
    const char *text;
    bool visible;
} plugin_ui_obj_ctx_t;

typedef struct {
    const char *title;
    const char *text;
} plugin_ui_popup_ctx_t;

static void plugin_api_lock(void) {
    if (!s_api_mutex) s_api_mutex = xSemaphoreCreateMutex();
    if (s_api_mutex) xSemaphoreTake(s_api_mutex, portMAX_DELAY);
}

static void plugin_api_unlock(void) {
    if (s_api_mutex) xSemaphoreGive(s_api_mutex);
}

static bool plugin_api_has_permission(uint32_t permission) {
    return (s_permissions & permission) != 0;
}

static bool plugin_api_has_ui_permission(void) {
    return plugin_api_has_permission(PLUGIN_PERMISSION_UI) || plugin_api_has_permission(PLUGIN_PERMISSION_LVGL);
}

static void plugin_ui_sync_apply(void *arg) {
    plugin_ui_sync_call_t *call = (plugin_ui_sync_call_t *)arg;
    if (call && call->fn) call->fn(call->ctx);
    if (call && call->done) xSemaphoreGive(call->done);
}

static bool plugin_ui_run_sync(void (*fn)(void *ctx), void *ctx) {
    if (!fn) return false;
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    if (!done) return false;
    plugin_ui_sync_call_t call = {
        .fn = fn,
        .ctx = ctx,
        .done = done,
    };
    display_manager_run_on_lvgl(plugin_ui_sync_apply, &call);
    bool ok = xSemaphoreTake(done, pdMS_TO_TICKS(1000)) == pdTRUE;
    vSemaphoreDelete(done);
    return ok;
}

static lv_obj_t *plugin_ui_parent_or_current(ghostesp_ui_obj_t parent) {
    if (parent && lv_obj_is_valid((lv_obj_t *)parent)) return (lv_obj_t *)parent;
    View *view = display_manager_get_current_view();
    if (view && view->root && lv_obj_is_valid(view->root)) return view->root;
    return lv_scr_act();
}

bool plugin_api_internal_has_ui_permission(void) {
    return plugin_api_has_ui_permission();
}

bool plugin_api_internal_run_sync(void (*fn)(void *ctx), void *ctx) {
    return plugin_ui_run_sync(fn, ctx);
}

lv_obj_t *plugin_api_internal_parent_or_current(ghostesp_ui_obj_t parent) {
    return plugin_ui_parent_or_current(parent);
}

static void plugin_ui_style_panel(lv_obj_t *obj) {
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x181818), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(obj, lv_color_hex(0x303030), LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(obj, GUI_RADIUS_MD, LV_PART_MAIN);
    lv_obj_set_style_pad_all(obj, GUI_GRID * 3, LV_PART_MAIN);
}

static void plugin_ui_style_button(lv_obj_t *obj) {
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x2B2B2B), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(obj, lv_color_hex(0x4A4A4A), LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(obj, GUI_RADIUS_MD, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(obj, GUI_GRID * 4, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(obj, GUI_GRID * 3, LV_PART_MAIN);
}

static void plugin_ui_button_event_cb(lv_event_t *event) {
    plugin_ui_button_ctx_t *ctx = (plugin_ui_button_ctx_t *)lv_event_get_user_data(event);
    if (!ctx) return;
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_CLICKED) {
        if (ctx->cb) ctx->cb(ctx->user);
    } else if (code == LV_EVENT_DELETE) {
        free(ctx);
    }
}

static void plugin_ui_delete_user_obj_event_cb(lv_event_t *event) {
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_user_data(event);
    if (obj && lv_obj_is_valid(obj)) lv_obj_del(obj);
}

static bool plugin_api_path_is_ghostesp_absolute(const char *path) {
    return path && strncmp(path, "/mnt/ghostesp/", 14) == 0;
}

static bool plugin_api_absolute_storage_allowed(const char *path) {
    return s_allow_absolute_storage && plugin_api_has_permission(PLUGIN_PERMISSION_STORAGE) && plugin_api_path_is_ghostesp_absolute(path);
}

static bool plugin_api_build_app_path(const char *path, char *out, size_t out_len) {
    if (!out || out_len == 0 || !plugin_api_has_permission(PLUGIN_PERMISSION_STORAGE) || s_app_data_path[0] == '\0') return false;
    if (!path || path[0] == '\0') {
        int n = snprintf(out, out_len, "%s", s_app_data_path);
        return n > 0 && (size_t)n < out_len;
    }
    if (path[0] == '/' || path[0] == '\\' || strstr(path, "..")) return false;
    int n = snprintf(out, out_len, "%s/%s", s_app_data_path, path);
    return n > 0 && (size_t)n < out_len;
}

static void plugin_api_log(const char *message) {
    if (!message) return;
    ESP_LOGI(TAG, "%s", message);
    glog("[app] %s\n", message);
}

static void plugin_api_ui_set_title(const char *title) {
    if (!plugin_api_has_permission(PLUGIN_PERMISSION_UI)) return;
    if (s_ui_set_title) s_ui_set_title(title ? title : "App");
}

static void plugin_api_ui_print(const char *text) {
    if (!plugin_api_has_permission(PLUGIN_PERMISSION_UI)) return;
    if (s_ui_print) s_ui_print(text ? text : "");
}

static void plugin_api_ui_clear(void) {
    if (!plugin_api_has_permission(PLUGIN_PERMISSION_UI)) return;
    if (s_ui_clear) s_ui_clear();
}

static void plugin_api_toast(const char *message) {
    if (!plugin_api_has_permission(PLUGIN_PERMISSION_UI)) return;
    if (s_ui_toast) s_ui_toast(message ? message : "");
    else if (message) glog("[app] %s\n", message);
}

static void plugin_api_ui_show_text(const char *title, const char *text) {
    if (!plugin_api_has_permission(PLUGIN_PERMISSION_UI)) return;
    plugin_api_ui_set_title(title);
    plugin_api_ui_clear();
    plugin_api_ui_print(text);
}

static void plugin_api_ui_screen_create_now(void *arg) {
    plugin_ui_create_ctx_t *ctx = (plugin_ui_create_ctx_t *)arg;
    lv_obj_t *root = plugin_ui_parent_or_current(NULL);
    if (!root) return;

    lv_obj_clean(root);
    lv_obj_set_style_bg_color(root, lv_color_hex(0x121212), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, LV_PART_MAIN);
    display_manager_add_status_bar(ctx->title && ctx->title[0] ? ctx->title : "SD App");

    lv_obj_t *content = gui_screen_create_content(root, GUI_STATUS_BAR_HEIGHT);
    if (!content) return;
    lv_obj_set_style_pad_all(content, GUI_GRID * 2, LV_PART_MAIN);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(content, GUI_GRID * 2, LV_PART_MAIN);
    ctx->result = content;
}

static ghostesp_ui_obj_t plugin_api_ui_screen_create(const char *title) {
    if (!plugin_api_has_ui_permission()) return NULL;
    plugin_ui_create_ctx_t ctx = { .title = title };
    return plugin_ui_run_sync(plugin_api_ui_screen_create_now, &ctx) ? ctx.result : NULL;
}

static void plugin_api_ui_card_create_now(void *arg) {
    plugin_ui_create_ctx_t *ctx = (plugin_ui_create_ctx_t *)arg;
    lv_obj_t *parent = plugin_ui_parent_or_current(ctx->parent);
    lv_obj_t *card = lv_obj_create(parent);
    if (!card) return;
    plugin_ui_style_panel(card);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, GUI_GRID * 2, LV_PART_MAIN);
    ctx->result = card;
}

static ghostesp_ui_obj_t plugin_api_ui_card_create(ghostesp_ui_obj_t parent) {
    if (!plugin_api_has_ui_permission()) return NULL;
    plugin_ui_create_ctx_t ctx = { .parent = parent };
    return plugin_ui_run_sync(plugin_api_ui_card_create_now, &ctx) ? ctx.result : NULL;
}

static void plugin_api_ui_label_create_now(void *arg) {
    plugin_ui_create_ctx_t *ctx = (plugin_ui_create_ctx_t *)arg;
    lv_obj_t *parent = plugin_ui_parent_or_current(ctx->parent);
    lv_obj_t *label = lv_label_create(parent);
    if (!label) return;
    lv_label_set_text(label, ctx->text ? ctx->text : "");
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, LV_PCT(100));
    lv_obj_set_style_text_color(label, lv_color_hex(0xE6E6E6), LV_PART_MAIN);
    lv_obj_set_style_text_font(label, gui_font_body(), LV_PART_MAIN);
    ctx->result = label;
}

static ghostesp_ui_obj_t plugin_api_ui_label_create(ghostesp_ui_obj_t parent, const char *text) {
    if (!plugin_api_has_ui_permission()) return NULL;
    plugin_ui_create_ctx_t ctx = { .parent = parent, .text = text };
    return plugin_ui_run_sync(plugin_api_ui_label_create_now, &ctx) ? ctx.result : NULL;
}

static void plugin_api_ui_button_create_now(void *arg) {
    plugin_ui_create_ctx_t *ctx = (plugin_ui_create_ctx_t *)arg;
    lv_obj_t *parent = plugin_ui_parent_or_current(ctx->parent);
    lv_obj_t *button = lv_btn_create(parent);
    if (!button) return;
    plugin_ui_style_button(button);
    lv_obj_set_width(button, LV_PCT(100));

    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, ctx->text ? ctx->text : "");
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_center(label);
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(label, gui_font_body(), LV_PART_MAIN);

    plugin_ui_button_ctx_t *button_ctx = calloc(1, sizeof(*button_ctx));
    if (button_ctx) {
        button_ctx->cb = ctx->cb;
        button_ctx->user = ctx->user;
        lv_obj_add_event_cb(button, plugin_ui_button_event_cb, LV_EVENT_ALL, button_ctx);
    }
    ctx->result = button;
}

static ghostesp_ui_obj_t plugin_api_ui_button_create(ghostesp_ui_obj_t parent, const char *text, ghostesp_ui_button_cb_t on_click, void *user) {
    if (!plugin_api_has_ui_permission()) return NULL;
    plugin_ui_create_ctx_t ctx = { .parent = parent, .text = text, .cb = on_click, .user = user };
    return plugin_ui_run_sync(plugin_api_ui_button_create_now, &ctx) ? ctx.result : NULL;
}

static void plugin_api_ui_label_set_text_now(void *arg) {
    plugin_ui_obj_ctx_t *ctx = (plugin_ui_obj_ctx_t *)arg;
    lv_obj_t *label = (lv_obj_t *)ctx->obj;
    if (label && lv_obj_is_valid(label)) lv_label_set_text(label, ctx->text ? ctx->text : "");
}

static void plugin_api_ui_label_set_text(ghostesp_ui_obj_t label, const char *text) {
    if (!plugin_api_has_ui_permission()) return;
    plugin_ui_obj_ctx_t ctx = { .obj = label, .text = text };
    plugin_ui_run_sync(plugin_api_ui_label_set_text_now, &ctx);
}

static void plugin_api_ui_button_set_text_now(void *arg) {
    plugin_ui_obj_ctx_t *ctx = (plugin_ui_obj_ctx_t *)arg;
    lv_obj_t *button = (lv_obj_t *)ctx->obj;
    if (!button || !lv_obj_is_valid(button) || lv_obj_get_child_cnt(button) == 0) return;
    lv_obj_t *label = lv_obj_get_child(button, 0);
    if (label && lv_obj_is_valid(label)) lv_label_set_text(label, ctx->text ? ctx->text : "");
}

static void plugin_api_ui_button_set_text(ghostesp_ui_obj_t button, const char *text) {
    if (!plugin_api_has_ui_permission()) return;
    plugin_ui_obj_ctx_t ctx = { .obj = button, .text = text };
    plugin_ui_run_sync(plugin_api_ui_button_set_text_now, &ctx);
}

static void plugin_api_ui_obj_set_visible_now(void *arg) {
    plugin_ui_obj_ctx_t *ctx = (plugin_ui_obj_ctx_t *)arg;
    lv_obj_t *obj = (lv_obj_t *)ctx->obj;
    if (!obj || !lv_obj_is_valid(obj)) return;
    if (ctx->visible) lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

static void plugin_api_ui_obj_set_visible(ghostesp_ui_obj_t obj, bool visible) {
    if (!plugin_api_has_ui_permission()) return;
    plugin_ui_obj_ctx_t ctx = { .obj = obj, .visible = visible };
    plugin_ui_run_sync(plugin_api_ui_obj_set_visible_now, &ctx);
}

static void plugin_api_ui_obj_delete_now(void *arg) {
    plugin_ui_obj_ctx_t *ctx = (plugin_ui_obj_ctx_t *)arg;
    lv_obj_t *obj = (lv_obj_t *)ctx->obj;
    if (obj && lv_obj_is_valid(obj)) lv_obj_del(obj);
}

static void plugin_api_ui_obj_delete(ghostesp_ui_obj_t obj) {
    if (!plugin_api_has_ui_permission()) return;
    plugin_ui_obj_ctx_t ctx = { .obj = obj };
    plugin_ui_run_sync(plugin_api_ui_obj_delete_now, &ctx);
}

static void plugin_api_ui_set_status_now(void *arg) {
    plugin_ui_obj_ctx_t *ctx = (plugin_ui_obj_ctx_t *)arg;
    display_manager_add_status_bar(ctx->text ? ctx->text : "SD App");
}

static void plugin_api_ui_set_status(const char *text) {
    if (!plugin_api_has_ui_permission()) return;
    plugin_ui_obj_ctx_t ctx = { .text = text };
    plugin_ui_run_sync(plugin_api_ui_set_status_now, &ctx);
}

static void plugin_api_ui_show_popup_now(void *arg) {
    plugin_ui_popup_ctx_t *ctx = (plugin_ui_popup_ctx_t *)arg;
    lv_obj_t *overlay = lv_obj_create(lv_scr_act());
    if (!overlay) return;
    lv_obj_set_size(overlay, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(overlay, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_60, LV_PART_MAIN);
    lv_obj_set_style_border_width(overlay, 0, LV_PART_MAIN);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *box = lv_obj_create(overlay);
    plugin_ui_style_panel(box);
    lv_obj_set_width(box, LV_PCT(82));
    lv_obj_center(box);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(box, GUI_GRID * 3, LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(box);
    lv_label_set_text(title, ctx->title ? ctx->title : "App");
    lv_obj_set_width(title, LV_PCT(100));
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, gui_font_title(), LV_PART_MAIN);

    lv_obj_t *body = lv_label_create(box);
    lv_label_set_text(body, ctx->text ? ctx->text : "");
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_style_text_color(body, lv_color_hex(0xD0D0D0), LV_PART_MAIN);
    lv_obj_set_style_text_font(body, gui_font_body(), LV_PART_MAIN);

    lv_obj_t *close = lv_btn_create(box);
    plugin_ui_style_button(close);
    lv_obj_set_width(close, LV_PCT(100));
    lv_obj_t *close_label = lv_label_create(close);
    lv_label_set_text(close_label, "Close");
    lv_obj_center(close_label);
    lv_obj_add_event_cb(close, plugin_ui_delete_user_obj_event_cb, LV_EVENT_CLICKED, overlay);
}

static void plugin_api_ui_show_popup(const char *title, const char *text) {
    if (!plugin_api_has_ui_permission()) return;
    plugin_ui_popup_ctx_t ctx = { .title = title, .text = text };
    plugin_ui_run_sync(plugin_api_ui_show_popup_now, &ctx);
}

static bool plugin_api_command_exec(const char *command) {
    if (!plugin_api_has_permission(PLUGIN_PERMISSION_COMMANDS)) return false;
    if (!command || command[0] == '\0') return false;
    if (!s_api_active) return false;
    simulateCommand(command);
    return true;
}

static bool plugin_api_storage_exists(const char *path) {
    if (!plugin_api_absolute_storage_allowed(path)) return false;
    return sd_card_exists(path);
}

static int plugin_api_storage_read(const char *path, void *buffer, size_t buffer_len) {
    if (!plugin_api_absolute_storage_allowed(path) || !buffer || buffer_len == 0) return -1;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t n = fread(buffer, 1, buffer_len, f);
    fclose(f);
    return (int)n;
}

static bool plugin_api_storage_write(const char *path, const void *data, size_t len) {
    if (!plugin_api_absolute_storage_allowed(path) || (!data && len > 0)) return false;
    return sd_card_write_file(path, data, len) == ESP_OK;
}

static bool plugin_api_storage_append(const char *path, const void *data, size_t len) {
    if (!plugin_api_absolute_storage_allowed(path) || (!data && len > 0)) return false;
    return sd_card_append_file(path, data, len) == ESP_OK;
}

static bool plugin_api_storage_delete(const char *path) {
    if (!plugin_api_absolute_storage_allowed(path)) return false;
    return unlink(path) == 0;
}

static bool plugin_api_storage_mkdir(const char *path) {
    if (!plugin_api_absolute_storage_allowed(path)) return false;
    return sd_card_create_directory(path) == ESP_OK;
}

static int plugin_api_storage_list(const char *path, ghostesp_storage_entry_t *out, int max_entries) {
    if (!plugin_api_absolute_storage_allowed(path) || !out || max_entries <= 0) return -1;
    DIR *dir = opendir(path);
    if (!dir) return -1;
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && count < max_entries) {
        if (entry->d_name[0] == '.') continue;
        strncpy(out[count].name, entry->d_name, GHOSTESP_STORAGE_NAME_MAX - 1);
        out[count].name[GHOSTESP_STORAGE_NAME_MAX - 1] = '\0';
        struct stat st;
        char full_path[256];
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
        snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);
#pragma GCC diagnostic pop
        out[count].is_directory = (stat(full_path, &st) == 0 && S_ISDIR(st.st_mode));
        count++;
    }
    closedir(dir);
    return count;
}

static const char *plugin_api_app_id(void) {
    return s_app_id;
}

static const char *plugin_api_app_data_path(void) {
    return s_app_data_path;
}

static bool plugin_api_app_storage_exists(const char *path) {
    char full_path[PLUGIN_APP_PATH_MAX];
    if (!plugin_api_build_app_path(path, full_path, sizeof(full_path))) return false;
    return sd_card_exists(full_path);
}

static int plugin_api_app_storage_read(const char *path, void *buffer, size_t buffer_len) {
    char full_path[PLUGIN_APP_PATH_MAX];
    if (!plugin_api_build_app_path(path, full_path, sizeof(full_path)) || !buffer || buffer_len == 0) return -1;
    FILE *f = fopen(full_path, "rb");
    if (!f) return -1;
    size_t n = fread(buffer, 1, buffer_len, f);
    fclose(f);
    return (int)n;
}

static bool plugin_api_app_storage_write(const char *path, const void *data, size_t len) {
    char full_path[PLUGIN_APP_PATH_MAX];
    if (!plugin_api_build_app_path(path, full_path, sizeof(full_path)) || (!data && len > 0)) return false;
    return sd_card_write_file(full_path, data, len) == ESP_OK;
}

static bool plugin_api_app_storage_append(const char *path, const void *data, size_t len) {
    char full_path[PLUGIN_APP_PATH_MAX];
    if (!plugin_api_build_app_path(path, full_path, sizeof(full_path)) || (!data && len > 0)) return false;
    return sd_card_append_file(full_path, data, len) == ESP_OK;
}

static bool plugin_api_app_storage_delete(const char *path) {
    char full_path[PLUGIN_APP_PATH_MAX];
    if (!plugin_api_build_app_path(path, full_path, sizeof(full_path))) return false;
    return unlink(full_path) == 0;
}

static bool plugin_api_app_storage_mkdir(const char *path) {
    char full_path[PLUGIN_APP_PATH_MAX];
    if (!plugin_api_build_app_path(path, full_path, sizeof(full_path))) return false;
    return sd_card_create_directory(full_path) == ESP_OK;
}

static int plugin_api_app_storage_list(const char *path, ghostesp_storage_entry_t *out, int max_entries) {
    char full_path[PLUGIN_APP_PATH_MAX];
    if (!plugin_api_build_app_path(path, full_path, sizeof(full_path)) || !out || max_entries <= 0) return -1;
    DIR *dir = opendir(full_path);
    if (!dir) return -1;
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && count < max_entries) {
        if (entry->d_name[0] == '.') continue;
        strncpy(out[count].name, entry->d_name, GHOSTESP_STORAGE_NAME_MAX - 1);
        out[count].name[GHOSTESP_STORAGE_NAME_MAX - 1] = '\0';
        struct stat st;
        char child_path[PLUGIN_APP_PATH_MAX];
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
        snprintf(child_path, sizeof(child_path), "%s/%s", full_path, entry->d_name);
#pragma GCC diagnostic pop
        out[count].is_directory = (stat(child_path, &st) == 0 && S_ISDIR(st.st_mode));
        count++;
    }
    closedir(dir);
    return count;
}

static bool plugin_api_badusb_run_script(const char *app_relative_path) {
    if (!plugin_api_has_permission(PLUGIN_PERMISSION_BADUSB)) return false;
#ifdef CONFIG_HAS_BADUSB
    char full_path[PLUGIN_APP_PATH_MAX];
    if (!plugin_api_build_app_path(app_relative_path, full_path, sizeof(full_path))) return false;
    return badusb_manager_execute_file(full_path) == ESP_OK;
#else
    return false;
#endif
}

static bool plugin_api_badusb_stop(void) {
    if (!plugin_api_has_permission(PLUGIN_PERMISSION_BADUSB)) return false;
#ifdef CONFIG_HAS_BADUSB
    return badusb_manager_stop() == ESP_OK;
#else
    return false;
#endif
}

static bool plugin_api_ir_send_file(const char *app_relative_path) {
    if (!plugin_api_has_permission(PLUGIN_PERMISSION_IR)) return false;
#ifdef CONFIG_HAS_INFRARED
    char full_path[PLUGIN_APP_PATH_MAX];
    infrared_signal_t signal = {0};
    if (!plugin_api_build_app_path(app_relative_path, full_path, sizeof(full_path))) return false;
    if (!infrared_manager_read_file(full_path, &signal)) return false;
    bool ok = infrared_manager_transmit(&signal);
    infrared_manager_free_signal(&signal);
    return ok;
#else
    return false;
#endif
}

static bool plugin_api_ir_stop(void) {
    if (!plugin_api_has_permission(PLUGIN_PERMISSION_IR)) return false;
#ifdef CONFIG_HAS_INFRARED
    infrared_manager_dazzler_stop();
    return true;
#else
    return false;
#endif
}

static bool plugin_api_nfc_is_available(void) {
#ifdef CONFIG_HAS_NFC
    return true;
#else
    return false;
#endif
}

static bool plugin_api_nfc_read_start(void) {
    if (!plugin_api_has_permission(PLUGIN_PERMISSION_NFC)) return false;
    return false;
}

static bool plugin_api_nfc_stop(void) {
    if (!plugin_api_has_permission(PLUGIN_PERMISSION_NFC)) return false;
    return false;
}

static bool plugin_api_ble_start_scan(void) {
    if (!plugin_api_has_permission(PLUGIN_PERMISSION_BLE)) return false;
#ifndef CONFIG_IDF_TARGET_ESP32S2
    return ble_start_scanning();
#else
    return false;
#endif
}

static bool plugin_api_ble_stop_scan(void) {
    if (!plugin_api_has_permission(PLUGIN_PERMISSION_BLE)) return false;
#ifndef CONFIG_IDF_TARGET_ESP32S2
    ble_stop();
    return true;
#else
    return false;
#endif
}

static int plugin_api_ble_device_count(void) {
    if (!plugin_api_has_permission(PLUGIN_PERMISSION_BLE)) return 0;
#ifndef CONFIG_IDF_TARGET_ESP32S2
    return ble_get_gatt_device_count();
#else
    return 0;
#endif
}

static bool plugin_api_ble_get_device(int index, ghostesp_ble_device_info_t *out) {
    if (!plugin_api_has_permission(PLUGIN_PERMISSION_BLE) || !out) return false;
#ifndef CONFIG_IDF_TARGET_ESP32S2
    memset(out, 0, sizeof(*out));
    return ble_get_gatt_device_data(index, out->mac, &out->rssi, out->name, sizeof(out->name)) == 0;
#else
    return false;
#endif
}

static bool plugin_api_subghz_is_available(void) {
#if defined(CONFIG_HAS_SUBGHZ) || defined(CONFIG_HAS_SUBGHZ_REMOTE)
    return subghz_remote_manager_is_ready();
#else
    return false;
#endif
}

static bool plugin_api_subghz_load_snapshot(const char *app_relative_path) {
    if (!plugin_api_has_permission(PLUGIN_PERMISSION_SUBGHZ)) return false;
#if defined(CONFIG_HAS_SUBGHZ) || defined(CONFIG_HAS_SUBGHZ_REMOTE)
    char full_path[PLUGIN_APP_PATH_MAX];
    if (!plugin_api_build_app_path(app_relative_path, full_path, sizeof(full_path))) return false;
    return subghz_remote_manager_load_snapshot(full_path);
#else
    return false;
#endif
}

static bool plugin_api_subghz_transmit_loaded(void) {
    if (!plugin_api_has_permission(PLUGIN_PERMISSION_SUBGHZ)) return false;
#if defined(CONFIG_HAS_SUBGHZ) || defined(CONFIG_HAS_SUBGHZ_REMOTE)
    return false;
#else
    return false;
#endif
}

static bool plugin_api_subghz_stop(void) {
    if (!plugin_api_has_permission(PLUGIN_PERMISSION_SUBGHZ)) return false;
#if defined(CONFIG_HAS_SUBGHZ) || defined(CONFIG_HAS_SUBGHZ_REMOTE)
    subghz_remote_manager_stop();
    return true;
#else
    return false;
#endif
}

static void plugin_api_delay_ms(uint32_t ms) {
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static void *plugin_api_app_malloc(size_t size) {
    if (size == 0) return NULL;
    if (s_memory_limit > 0 && s_memory_used + size > s_memory_limit) return NULL;
    plugin_alloc_header_t *header = malloc(sizeof(*header) + size);
    if (!header) return NULL;
    header->magic = PLUGIN_ALLOC_MAGIC;
    header->size = size;
    s_memory_used += size;
    return header + 1;
}

static void *plugin_api_app_calloc(size_t count, size_t size) {
    if (count == 0 || size == 0) return NULL;
    if (count > SIZE_MAX / size) return NULL;
    size_t total = count * size;
    void *ptr = plugin_api_app_malloc(total);
    if (ptr) memset(ptr, 0, total);
    return ptr;
}

static void plugin_api_app_free(void *ptr) {
    if (!ptr) return;
    plugin_alloc_header_t *header = ((plugin_alloc_header_t *)ptr) - 1;
    if (header->magic != PLUGIN_ALLOC_MAGIC) return;
    if (s_memory_used >= header->size) s_memory_used -= header->size;
    else s_memory_used = 0;
    header->magic = 0;
    free(header);
}

static size_t plugin_api_app_memory_used(void) {
    return s_memory_used;
}

static size_t plugin_api_app_memory_limit(void) {
    return s_memory_limit;
}

static size_t plugin_api_system_free_heap(void) {
    return heap_caps_get_free_size(MALLOC_CAP_8BIT);
}

static size_t plugin_api_system_free_internal_heap(void) {
    return heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
}

static uint32_t plugin_api_system_uptime_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static const char *plugin_api_system_firmware_version(void) {
    const esp_app_desc_t *desc = esp_app_get_description();
    return desc ? desc->version : "unknown";
}

static const char *plugin_api_system_target(void) {
    return CONFIG_IDF_TARGET;
}

static bool plugin_api_wifi_start_scan(void) {
    if (!plugin_api_has_permission(PLUGIN_PERMISSION_WIFI)) return false;
    wifi_manager_start_scan();
    return true;
}

static bool plugin_api_wifi_stop_scan(void) {
    if (!plugin_api_has_permission(PLUGIN_PERMISSION_WIFI)) return false;
    wifi_manager_stop_scan();
    return true;
}

static uint16_t plugin_api_wifi_ap_count(void) {
    if (!plugin_api_has_permission(PLUGIN_PERMISSION_WIFI)) return 0;
    return ap_count;
}

static bool plugin_api_wifi_scan_get_ap(uint16_t index, ghostesp_wifi_ap_info_t *out) {
    if (!plugin_api_has_permission(PLUGIN_PERMISSION_WIFI)) return false;
    if (!out || index >= ap_count || !scanned_aps) return false;
    wifi_ap_record_t *ap = &scanned_aps[index];
    memcpy(out->bssid, ap->bssid, 6);
    memcpy(out->ssid, ap->ssid, 32);
    out->ssid[32] = '\0';
    out->channel = ap->primary;
    out->rssi = ap->rssi;
    out->auth_mode = (uint8_t)ap->authmode;
    return true;
}

static bool plugin_api_rgb_set_all(uint8_t red, uint8_t green, uint8_t blue) {
    if (!plugin_api_has_permission(PLUGIN_PERMISSION_RGB)) return false;
    return rgb_manager_set_color(&rgb_manager, -1, red, green, blue, false) == ESP_OK;
}

const char *plugin_api_current_target(void) {
    return CONFIG_IDF_TARGET;
}

static void *plugin_unsafe_lv_scr_act(void) {
    return lv_scr_act();
}

static void *plugin_unsafe_display_get_current_view(void) {
    return display_manager_get_current_view();
}

static void *plugin_unsafe_raw_symbol(const char *name) {
    if (!name) return NULL;
#if CONFIG_ENABLE_NATIVE_SD_APPS
    return (void *)esp_elf_find_symbol(name);
#else
    return NULL;
#endif
}

extern bool plugin_api_gps_is_available(void);
extern bool plugin_api_gps_has_fix(void);
extern double plugin_api_gps_get_latitude(void);
extern double plugin_api_gps_get_longitude(void);
extern double plugin_api_gps_get_altitude(void);
extern int plugin_api_gps_get_satellites(void);
extern float plugin_api_gps_get_speed(void);
extern float plugin_api_gps_get_heading(void);
extern uint8_t plugin_api_settings_get_theme(void);
extern const char *plugin_api_settings_get_device_name(void);
extern void plugin_api_ui_input_dialog(const char *title, const char *default_text,
                                       ghostesp_input_submit_cb_t on_submit, void *user);

extern uint32_t plugin_api_ui_theme_get_background(void);
extern uint32_t plugin_api_ui_theme_get_surface(void);
extern uint32_t plugin_api_ui_theme_get_surface_alt(void);
extern uint32_t plugin_api_ui_theme_get_text(void);
extern uint32_t plugin_api_ui_theme_get_text_muted(void);
extern uint32_t plugin_api_ui_theme_get_accent(void);
extern bool plugin_api_ui_theme_is_bright(void);
extern void plugin_api_ui_obj_set_bg_color(ghostesp_ui_obj_t obj, uint32_t hex_color);
extern void plugin_api_ui_obj_set_text_color(ghostesp_ui_obj_t obj, uint32_t hex_color);
extern void plugin_api_ui_obj_set_border_color(ghostesp_ui_obj_t obj, uint32_t hex_color);
extern void plugin_api_ui_obj_set_border_width(ghostesp_ui_obj_t obj, int32_t width);
extern void plugin_api_ui_obj_set_radius(ghostesp_ui_obj_t obj, int32_t radius);
extern void plugin_api_ui_obj_set_pad(ghostesp_ui_obj_t obj, int32_t left, int32_t right, int32_t top, int32_t bottom);
extern void plugin_api_ui_obj_set_font(ghostesp_ui_obj_t obj, ghostesp_font_size_t size);
extern void plugin_api_ui_obj_set_opa(ghostesp_ui_obj_t obj, uint8_t opa);
extern void plugin_api_ui_obj_set_pos(ghostesp_ui_obj_t obj, int32_t x, int32_t y);
extern void plugin_api_ui_obj_set_size(ghostesp_ui_obj_t obj, int32_t w, int32_t h);
extern void plugin_api_ui_obj_set_width(ghostesp_ui_obj_t obj, int32_t w);
extern void plugin_api_ui_obj_set_height(ghostesp_ui_obj_t obj, int32_t h);
extern void plugin_api_ui_obj_align(ghostesp_ui_obj_t obj, ghostesp_align_t align, int32_t x_ofs, int32_t y_ofs);
extern int32_t plugin_api_ui_obj_get_width(ghostesp_ui_obj_t obj);
extern int32_t plugin_api_ui_obj_get_height(ghostesp_ui_obj_t obj);
extern int32_t plugin_api_ui_obj_get_x(ghostesp_ui_obj_t obj);
extern int32_t plugin_api_ui_obj_get_y(ghostesp_ui_obj_t obj);
extern void plugin_api_ui_obj_set_flex_flow(ghostesp_ui_obj_t obj, ghostesp_flex_flow_t flow);
extern void plugin_api_ui_obj_set_flex_align(ghostesp_ui_obj_t obj, ghostesp_flex_align_t main, ghostesp_flex_align_t cross, ghostesp_flex_align_t track);
extern void plugin_api_ui_obj_set_flex_grow(ghostesp_ui_obj_t obj, uint8_t grow);
extern void plugin_api_ui_obj_set_pad_row(ghostesp_ui_obj_t obj, int32_t pad);
extern void plugin_api_ui_obj_set_pad_column(ghostesp_ui_obj_t obj, int32_t pad);
extern int32_t plugin_api_ui_screen_get_width(void);
extern int32_t plugin_api_ui_screen_get_height(void);

extern ghostesp_options_t plugin_api_ui_options_create(const char *title);
extern ghostesp_ui_obj_t plugin_api_ui_options_add_item(ghostesp_options_t opts, const char *label, ghostesp_ui_button_cb_t on_click, void *user);
extern ghostesp_ui_obj_t plugin_api_ui_options_add_back(ghostesp_options_t opts, ghostesp_ui_button_cb_t on_click, void *user);
extern void plugin_api_ui_options_set_selected(ghostesp_options_t opts, int index);
extern void plugin_api_ui_options_move_selection(ghostesp_options_t opts, int delta);
extern int plugin_api_ui_options_get_selected(ghostesp_options_t opts);
extern void plugin_api_ui_options_clear(ghostesp_options_t opts);
extern void plugin_api_ui_options_destroy(ghostesp_options_t opts);

extern ghostesp_detail_t plugin_api_ui_detail_create(const char *title);
extern void plugin_api_ui_detail_add_info(ghostesp_detail_t dv, const char *label, const char *value);
extern void plugin_api_ui_detail_add_action(ghostesp_detail_t dv, const char *label, ghostesp_ui_button_cb_t on_click, void *user);
extern void plugin_api_ui_detail_add_header(ghostesp_detail_t dv, const char *text);
extern void plugin_api_ui_detail_add_divider(ghostesp_detail_t dv);
extern ghostesp_ui_obj_t plugin_api_ui_detail_add_back(ghostesp_detail_t dv, ghostesp_ui_button_cb_t on_click, void *user);
extern void plugin_api_ui_detail_set_selected(ghostesp_detail_t dv, int index);
extern void plugin_api_ui_detail_move_selection(ghostesp_detail_t dv, int delta);
extern int plugin_api_ui_detail_get_selected(ghostesp_detail_t dv);
extern int plugin_api_ui_detail_get_count(ghostesp_detail_t dv);
extern void plugin_api_ui_detail_clear(ghostesp_detail_t dv);
extern void plugin_api_ui_detail_destroy(ghostesp_detail_t dv);

extern ghostesp_popup_t plugin_api_ui_popup_create(int32_t width, int32_t height);
extern void plugin_api_ui_popup_set_title(ghostesp_popup_t p, const char *title);
extern void plugin_api_ui_popup_set_body(ghostesp_popup_t p, const char *body);
extern ghostesp_ui_obj_t plugin_api_ui_popup_add_button(ghostesp_popup_t p, const char *label, ghostesp_ui_button_cb_t on_click, void *user);
extern void plugin_api_ui_popup_show(ghostesp_popup_t p);
extern void plugin_api_ui_popup_hide(ghostesp_popup_t p);
extern void plugin_api_ui_popup_destroy(ghostesp_popup_t p);

extern ghostesp_scan_t plugin_api_ui_scan_status_create(const char *message);
extern void plugin_api_ui_scan_status_update(ghostesp_scan_t ss, const char *message);
extern void plugin_api_ui_scan_status_set_progress(ghostesp_scan_t ss, int current, int total);
extern void plugin_api_ui_scan_status_close(ghostesp_scan_t ss);

extern ghostesp_ui_obj_t plugin_api_ui_canvas_create(ghostesp_ui_obj_t parent, int32_t width, int32_t height);
extern void plugin_api_ui_canvas_draw_rect(ghostesp_ui_obj_t canvas, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t hex_color);
extern void plugin_api_ui_canvas_fill(ghostesp_ui_obj_t canvas, uint32_t hex_color);
extern void plugin_api_ui_canvas_draw_line(ghostesp_ui_obj_t canvas, const ghostesp_point_t *points, int count, uint32_t hex_color, int32_t width);
extern void plugin_api_ui_canvas_draw_arc(ghostesp_ui_obj_t canvas, int32_t cx, int32_t cy, int32_t r, int32_t start_angle, int32_t end_angle, uint32_t hex_color, int32_t width);

extern void plugin_api_ui_anim_slide_in(ghostesp_ui_obj_t obj, int direction, uint32_t duration_ms);
extern void plugin_api_ui_anim_slide_out(ghostesp_ui_obj_t obj, int direction, uint32_t duration_ms, ghostesp_anim_done_cb_t on_done, void *user);
extern void plugin_api_ui_anim_pop_in(ghostesp_ui_obj_t obj);
extern void plugin_api_ui_anim_press_pulse(ghostesp_ui_obj_t obj);

extern ghostesp_ui_obj_t plugin_api_ui_arc_create(ghostesp_ui_obj_t parent);
extern void plugin_api_ui_arc_set_value(ghostesp_ui_obj_t arc, int32_t value);
extern void plugin_api_ui_arc_set_range(ghostesp_ui_obj_t arc, int32_t min, int32_t max);
extern void plugin_api_ui_arc_set_angles(ghostesp_ui_obj_t arc, int32_t start, int32_t end);
extern void plugin_api_ui_arc_set_bg_angles(ghostesp_ui_obj_t arc, int32_t start, int32_t end);
extern void plugin_api_ui_arc_set_bg_color(ghostesp_ui_obj_t arc, uint32_t hex_color);
extern void plugin_api_ui_arc_set_indicator_color(ghostesp_ui_obj_t arc, uint32_t hex_color);

extern ghostesp_ui_obj_t plugin_api_ui_line_create(ghostesp_ui_obj_t parent);
extern void plugin_api_ui_line_set_points(ghostesp_ui_obj_t line, const ghostesp_point_t *points, int count);
extern void plugin_api_ui_line_set_color(ghostesp_ui_obj_t line, uint32_t hex_color);
extern void plugin_api_ui_line_set_width(ghostesp_ui_obj_t line, int32_t width);

extern ghostesp_ui_obj_t plugin_api_ui_image_create(ghostesp_ui_obj_t parent);
extern bool plugin_api_ui_image_set_src(ghostesp_ui_obj_t img, const char *app_relative_path);

extern ghostesp_ui_timer_t plugin_api_ui_timer_create(ghostesp_ui_timer_cb_t cb, uint32_t interval_ms, void *user);
extern void plugin_api_ui_timer_delete(ghostesp_ui_timer_t timer);
extern void plugin_api_ui_timer_set_interval(ghostesp_ui_timer_t timer, uint32_t interval_ms);

extern ghostesp_paged_menu_t plugin_api_ui_paged_menu_create(int page_size, ghostesp_paged_menu_load_fn load_fn, void *user);
extern void plugin_api_ui_paged_menu_set_callbacks(ghostesp_paged_menu_t pm, ghostesp_paged_menu_select_fn select_fn, ghostesp_paged_menu_nav_fn prev_fn, ghostesp_paged_menu_nav_fn next_fn, void *user);
extern void plugin_api_ui_paged_menu_reset(ghostesp_paged_menu_t pm);
extern void plugin_api_ui_paged_menu_destroy(ghostesp_paged_menu_t pm);
extern bool plugin_api_ui_paged_menu_has_prev(ghostesp_paged_menu_t pm);
extern bool plugin_api_ui_paged_menu_has_next(ghostesp_paged_menu_t pm);

static ghostesp_api_t s_api = {
    .api_version = GHOSTESP_APP_API_VERSION,
    .struct_size = GHOSTESP_API_STRUCT_SIZE_V1,
    .target = CONFIG_IDF_TARGET,
    .log = plugin_api_log,
    .ui_set_title = plugin_api_ui_set_title,
    .ui_print = plugin_api_ui_print,
    .ui_clear = plugin_api_ui_clear,
    .toast = plugin_api_toast,
    .ui_show_text = plugin_api_ui_show_text,
    .command_exec = plugin_api_command_exec,
    .storage_exists = plugin_api_storage_exists,
    .storage_read = plugin_api_storage_read,
    .storage_write = plugin_api_storage_write,
    .storage_append = plugin_api_storage_append,
    .storage_delete = plugin_api_storage_delete,
    .storage_mkdir = plugin_api_storage_mkdir,
    .storage_list = plugin_api_storage_list,
    .malloc = plugin_api_app_malloc,
    .free = plugin_api_app_free,
    .app_malloc = plugin_api_app_malloc,
    .app_calloc = plugin_api_app_calloc,
    .app_free = plugin_api_app_free,
    .app_memory_used = plugin_api_app_memory_used,
    .app_memory_limit = plugin_api_app_memory_limit,
    .delay_ms = plugin_api_delay_ms,
    .system_free_heap = plugin_api_system_free_heap,
    .system_free_internal_heap = plugin_api_system_free_internal_heap,
    .system_uptime_ms = plugin_api_system_uptime_ms,
    .system_firmware_version = plugin_api_system_firmware_version,
    .system_target = plugin_api_system_target,
    .wifi_start_scan = plugin_api_wifi_start_scan,
    .wifi_stop_scan = plugin_api_wifi_stop_scan,
    .wifi_ap_count = plugin_api_wifi_ap_count,
    .wifi_scan_get_ap = plugin_api_wifi_scan_get_ap,
    .rgb_set_all = plugin_api_rgb_set_all,
    .lv_scr_act = plugin_unsafe_lv_scr_act,
    .display_get_current_view = plugin_unsafe_display_get_current_view,
    .raw_symbol = plugin_unsafe_raw_symbol,
    .app_id = plugin_api_app_id,
    .app_data_path = plugin_api_app_data_path,
    .app_storage_exists = plugin_api_app_storage_exists,
    .app_storage_read = plugin_api_app_storage_read,
    .app_storage_write = plugin_api_app_storage_write,
    .app_storage_append = plugin_api_app_storage_append,
    .app_storage_delete = plugin_api_app_storage_delete,
    .app_storage_mkdir = plugin_api_app_storage_mkdir,
    .app_storage_list = plugin_api_app_storage_list,
    .badusb_run_script = plugin_api_badusb_run_script,
    .badusb_stop = plugin_api_badusb_stop,
    .ir_send_file = plugin_api_ir_send_file,
    .ir_stop = plugin_api_ir_stop,
    .nfc_is_available = plugin_api_nfc_is_available,
    .nfc_read_start = plugin_api_nfc_read_start,
    .nfc_stop = plugin_api_nfc_stop,
    .ble_start_scan = plugin_api_ble_start_scan,
    .ble_stop_scan = plugin_api_ble_stop_scan,
    .ble_device_count = plugin_api_ble_device_count,
    .ble_get_device = plugin_api_ble_get_device,
    .subghz_is_available = plugin_api_subghz_is_available,
    .subghz_load_snapshot = plugin_api_subghz_load_snapshot,
    .subghz_transmit_loaded = plugin_api_subghz_transmit_loaded,
    .subghz_stop = plugin_api_subghz_stop,
    .ui_screen_create = plugin_api_ui_screen_create,
    .ui_card_create = plugin_api_ui_card_create,
    .ui_label_create = plugin_api_ui_label_create,
    .ui_button_create = plugin_api_ui_button_create,
    .ui_label_set_text = plugin_api_ui_label_set_text,
    .ui_button_set_text = plugin_api_ui_button_set_text,
    .ui_obj_set_visible = plugin_api_ui_obj_set_visible,
    .ui_obj_delete = plugin_api_ui_obj_delete,
    .ui_set_status = plugin_api_ui_set_status,
    .ui_show_popup = plugin_api_ui_show_popup,
    .ui_theme_get_background = plugin_api_ui_theme_get_background,
    .ui_theme_get_surface = plugin_api_ui_theme_get_surface,
    .ui_theme_get_surface_alt = plugin_api_ui_theme_get_surface_alt,
    .ui_theme_get_text = plugin_api_ui_theme_get_text,
    .ui_theme_get_text_muted = plugin_api_ui_theme_get_text_muted,
    .ui_theme_get_accent = plugin_api_ui_theme_get_accent,
    .ui_theme_is_bright = plugin_api_ui_theme_is_bright,
    .ui_obj_set_bg_color = plugin_api_ui_obj_set_bg_color,
    .ui_obj_set_text_color = plugin_api_ui_obj_set_text_color,
    .ui_obj_set_border_color = plugin_api_ui_obj_set_border_color,
    .ui_obj_set_border_width = plugin_api_ui_obj_set_border_width,
    .ui_obj_set_radius = plugin_api_ui_obj_set_radius,
    .ui_obj_set_pad = plugin_api_ui_obj_set_pad,
    .ui_obj_set_font = plugin_api_ui_obj_set_font,
    .ui_obj_set_opa = plugin_api_ui_obj_set_opa,
    .ui_obj_set_pos = plugin_api_ui_obj_set_pos,
    .ui_obj_set_size = plugin_api_ui_obj_set_size,
    .ui_obj_set_width = plugin_api_ui_obj_set_width,
    .ui_obj_set_height = plugin_api_ui_obj_set_height,
    .ui_obj_align = plugin_api_ui_obj_align,
    .ui_obj_get_width = plugin_api_ui_obj_get_width,
    .ui_obj_get_height = plugin_api_ui_obj_get_height,
    .ui_obj_get_x = plugin_api_ui_obj_get_x,
    .ui_obj_get_y = plugin_api_ui_obj_get_y,
    .ui_obj_set_flex_flow = plugin_api_ui_obj_set_flex_flow,
    .ui_obj_set_flex_align = plugin_api_ui_obj_set_flex_align,
    .ui_obj_set_flex_grow = plugin_api_ui_obj_set_flex_grow,
    .ui_obj_set_pad_row = plugin_api_ui_obj_set_pad_row,
    .ui_obj_set_pad_column = plugin_api_ui_obj_set_pad_column,
    .ui_timer_create = plugin_api_ui_timer_create,
    .ui_timer_delete = plugin_api_ui_timer_delete,
    .ui_timer_set_interval = plugin_api_ui_timer_set_interval,
    .ui_options_create = plugin_api_ui_options_create,
    .ui_options_add_item = plugin_api_ui_options_add_item,
    .ui_options_add_back = plugin_api_ui_options_add_back,
    .ui_options_set_selected = plugin_api_ui_options_set_selected,
    .ui_options_move_selection = plugin_api_ui_options_move_selection,
    .ui_options_get_selected = plugin_api_ui_options_get_selected,
    .ui_options_clear = plugin_api_ui_options_clear,
    .ui_options_destroy = plugin_api_ui_options_destroy,
    .ui_detail_create = plugin_api_ui_detail_create,
    .ui_detail_add_info = plugin_api_ui_detail_add_info,
    .ui_detail_add_action = plugin_api_ui_detail_add_action,
    .ui_detail_add_header = plugin_api_ui_detail_add_header,
    .ui_detail_add_divider = plugin_api_ui_detail_add_divider,
    .ui_detail_add_back = plugin_api_ui_detail_add_back,
    .ui_detail_set_selected = plugin_api_ui_detail_set_selected,
    .ui_detail_move_selection = plugin_api_ui_detail_move_selection,
    .ui_detail_get_selected = plugin_api_ui_detail_get_selected,
    .ui_detail_get_count = plugin_api_ui_detail_get_count,
    .ui_detail_clear = plugin_api_ui_detail_clear,
    .ui_detail_destroy = plugin_api_ui_detail_destroy,
    .ui_popup_create = plugin_api_ui_popup_create,
    .ui_popup_set_title = plugin_api_ui_popup_set_title,
    .ui_popup_set_body = plugin_api_ui_popup_set_body,
    .ui_popup_add_button = plugin_api_ui_popup_add_button,
    .ui_popup_show = plugin_api_ui_popup_show,
    .ui_popup_hide = plugin_api_ui_popup_hide,
    .ui_popup_destroy = plugin_api_ui_popup_destroy,
    .ui_scan_status_create = plugin_api_ui_scan_status_create,
    .ui_scan_status_update = plugin_api_ui_scan_status_update,
    .ui_scan_status_set_progress = plugin_api_ui_scan_status_set_progress,
    .ui_scan_status_close = plugin_api_ui_scan_status_close,
    .ui_canvas_create = plugin_api_ui_canvas_create,
    .ui_canvas_draw_rect = plugin_api_ui_canvas_draw_rect,
    .ui_canvas_fill = plugin_api_ui_canvas_fill,
    .ui_canvas_draw_line = plugin_api_ui_canvas_draw_line,
    .ui_canvas_draw_arc = plugin_api_ui_canvas_draw_arc,
    .ui_anim_slide_in = plugin_api_ui_anim_slide_in,
    .ui_anim_slide_out = plugin_api_ui_anim_slide_out,
    .ui_anim_pop_in = plugin_api_ui_anim_pop_in,
    .ui_anim_press_pulse = plugin_api_ui_anim_press_pulse,
    .ui_arc_create = plugin_api_ui_arc_create,
    .ui_arc_set_value = plugin_api_ui_arc_set_value,
    .ui_arc_set_range = plugin_api_ui_arc_set_range,
    .ui_arc_set_angles = plugin_api_ui_arc_set_angles,
    .ui_arc_set_bg_angles = plugin_api_ui_arc_set_bg_angles,
    .ui_arc_set_bg_color = plugin_api_ui_arc_set_bg_color,
    .ui_arc_set_indicator_color = plugin_api_ui_arc_set_indicator_color,
    .ui_line_create = plugin_api_ui_line_create,
    .ui_line_set_points = plugin_api_ui_line_set_points,
    .ui_line_set_color = plugin_api_ui_line_set_color,
    .ui_line_set_width = plugin_api_ui_line_set_width,
    .ui_image_create = plugin_api_ui_image_create,
    .ui_image_set_src = plugin_api_ui_image_set_src,
    .ui_paged_menu_create = plugin_api_ui_paged_menu_create,
    .ui_paged_menu_set_callbacks = plugin_api_ui_paged_menu_set_callbacks,
    .ui_paged_menu_reset = plugin_api_ui_paged_menu_reset,
    .ui_paged_menu_destroy = plugin_api_ui_paged_menu_destroy,
    .ui_paged_menu_has_prev = plugin_api_ui_paged_menu_has_prev,
    .ui_paged_menu_has_next = plugin_api_ui_paged_menu_has_next,
    .gps_is_available = plugin_api_gps_is_available,
    .gps_has_fix = plugin_api_gps_has_fix,
    .gps_get_latitude = plugin_api_gps_get_latitude,
    .gps_get_longitude = plugin_api_gps_get_longitude,
    .gps_get_altitude = plugin_api_gps_get_altitude,
    .gps_get_satellites = plugin_api_gps_get_satellites,
    .gps_get_speed = plugin_api_gps_get_speed,
    .gps_get_heading = plugin_api_gps_get_heading,
    .settings_get_theme = plugin_api_settings_get_theme,
    .settings_get_device_name = plugin_api_settings_get_device_name,
    .ui_input_dialog = plugin_api_ui_input_dialog,
    .ui_screen_get_width = plugin_api_ui_screen_get_width,
    .ui_screen_get_height = plugin_api_ui_screen_get_height,
};

const ghostesp_api_t *plugin_api_get(const char *app_id,
                                     uint32_t permissions,
                                     size_t memory_limit,
                                     bool allow_absolute_storage) {
    plugin_api_lock();
    if (s_api_active) {
        plugin_api_unlock();
        ESP_LOGE(TAG, "plugin_api_get called while another app is active");
        return NULL;
    }
    s_permissions = permissions;
    s_allow_absolute_storage = allow_absolute_storage;
    s_memory_limit = memory_limit;
    s_memory_used = 0;
    s_app_id[0] = '\0';
    s_app_data_path[0] = '\0';
    if (app_id) {
        for (const char *p = app_id; *p; ++p) {
            if (!(isalnum((unsigned char)*p) || *p == '_' || *p == '-')) {
                plugin_api_unlock();
                ESP_LOGE(TAG, "plugin_api_get: invalid app_id");
                return NULL;
            }
        }
        strncpy(s_app_id, app_id, sizeof(s_app_id) - 1);
        s_app_id[sizeof(s_app_id) - 1] = '\0';
        snprintf(s_app_data_path, sizeof(s_app_data_path), "/mnt/ghostesp/appdata/%s", s_app_id);
        sd_card_create_directory("/mnt/ghostesp/appdata");
        sd_card_create_directory(s_app_data_path);
    }

    s_api.flags = GHOSTESP_APP_FLAG_PERMISSIONS_ENFORCED;
    if (allow_absolute_storage) s_api.flags |= GHOSTESP_APP_FLAG_ABSOLUTE_STORAGE_ALLOWED;
    s_api_active = true;
    plugin_api_unlock();
    return &s_api;
}

void plugin_api_set_ui_hooks(void (*set_title)(const char *title),
                             void (*print)(const char *text),
                             void (*clear)(void),
                             void (*toast)(const char *message)) {
    s_ui_set_title = set_title;
    s_ui_print = print;
    s_ui_clear = clear;
    s_ui_toast = toast;
}

bool plugin_api_is_active(void) {
    return s_api_active;
}

void plugin_api_release(void) {
    plugin_api_lock();
    s_api_active = false;
    s_permissions = 0;
    s_allow_absolute_storage = false;
    s_memory_limit = 0;
    s_memory_used = 0;
    s_app_id[0] = '\0';
    s_app_data_path[0] = '\0';
    plugin_api_unlock();
}
