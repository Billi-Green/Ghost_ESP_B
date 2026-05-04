#include "managers/plugin_api.h"

#include "core/serial_manager.h"
#include "core/glog.h"
#include "managers/display_manager.h"
#include "managers/rgb_manager.h"
#include "managers/sd_card_manager.h"
#include "managers/wifi_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "esp_log.h"
#include "sdkconfig.h"
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

static void plugin_api_log(const char *message) {
    if (!message) return;
    ESP_LOGI(TAG, "%s", message);
    glog("[app] %s\n", message);
}

static void plugin_api_ui_set_title(const char *title) {
    if (s_ui_set_title) s_ui_set_title(title ? title : "App");
}

static void plugin_api_ui_print(const char *text) {
    if (s_ui_print) s_ui_print(text ? text : "");
}

static void plugin_api_ui_clear(void) {
    if (s_ui_clear) s_ui_clear();
}

static void plugin_api_toast(const char *message) {
    if (s_ui_toast) s_ui_toast(message ? message : "");
    else if (message) glog("[app] %s\n", message);
}

static bool plugin_api_command_exec(const char *command) {
    if (!command || command[0] == '\0') return false;
    simulateCommand(command);
    return true;
}

static bool plugin_api_storage_exists(const char *path) {
    if (!path || strncmp(path, "/mnt/ghostesp/", 14) != 0) return false;
    return sd_card_exists(path);
}

static int plugin_api_storage_read(const char *path, void *buffer, size_t buffer_len) {
    if (!path || !buffer || buffer_len == 0 || strncmp(path, "/mnt/ghostesp/", 14) != 0) return -1;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t n = fread(buffer, 1, buffer_len, f);
    fclose(f);
    return (int)n;
}

static bool plugin_api_storage_write(const char *path, const void *data, size_t len) {
    if (!path || (!data && len > 0) || strncmp(path, "/mnt/ghostesp/", 14) != 0) return false;
    return sd_card_write_file(path, data, len) == ESP_OK;
}

static bool plugin_api_storage_append(const char *path, const void *data, size_t len) {
    if (!path || (!data && len > 0) || strncmp(path, "/mnt/ghostesp/", 14) != 0) return false;
    return sd_card_append_file(path, data, len) == ESP_OK;
}

static bool plugin_api_storage_delete(const char *path) {
    if (!path || strncmp(path, "/mnt/ghostesp/", 14) != 0) return false;
    return unlink(path) == 0;
}

static bool plugin_api_storage_mkdir(const char *path) {
    if (!path || strncmp(path, "/mnt/ghostesp/", 14) != 0) return false;
    return sd_card_create_directory(path) == ESP_OK;
}

static int plugin_api_storage_list(const char *path, ghostesp_storage_entry_t *out, int max_entries) {
    if (!path || !out || max_entries <= 0 || strncmp(path, "/mnt/ghostesp/", 14) != 0) return -1;
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

static void plugin_api_delay_ms(uint32_t ms) {
    vTaskDelay(pdMS_TO_TICKS(ms));
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

static bool plugin_api_wifi_start_scan(void) {
    wifi_manager_start_scan();
    return true;
}

static bool plugin_api_wifi_stop_scan(void) {
    wifi_manager_stop_scan();
    return true;
}

static uint16_t plugin_api_wifi_ap_count(void) {
    return ap_count;
}

static bool plugin_api_wifi_scan_get_ap(uint16_t index, ghostesp_wifi_ap_info_t *out) {
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

static const ghostesp_unsafe_api_t s_unsafe_api = {
    .api_version = GHOSTESP_APP_API_VERSION,
    .lv_scr_act = plugin_unsafe_lv_scr_act,
    .display_get_current_view = plugin_unsafe_display_get_current_view,
    .raw_symbol = plugin_unsafe_raw_symbol,
};

static ghostesp_api_t s_api = {
    .api_version = GHOSTESP_APP_API_VERSION,
    .target = CONFIG_IDF_TARGET,
    .log = plugin_api_log,
    .ui_set_title = plugin_api_ui_set_title,
    .ui_print = plugin_api_ui_print,
    .ui_clear = plugin_api_ui_clear,
    .toast = plugin_api_toast,
    .command_exec = plugin_api_command_exec,
    .storage_exists = plugin_api_storage_exists,
    .storage_read = plugin_api_storage_read,
    .storage_write = plugin_api_storage_write,
    .storage_append = plugin_api_storage_append,
    .storage_delete = plugin_api_storage_delete,
    .storage_mkdir = plugin_api_storage_mkdir,
    .storage_list = plugin_api_storage_list,
    .malloc = malloc,
    .free = free,
    .delay_ms = plugin_api_delay_ms,
    .system_free_heap = plugin_api_system_free_heap,
    .system_free_internal_heap = plugin_api_system_free_internal_heap,
    .system_uptime_ms = plugin_api_system_uptime_ms,
    .wifi_start_scan = plugin_api_wifi_start_scan,
    .wifi_stop_scan = plugin_api_wifi_stop_scan,
    .wifi_ap_count = plugin_api_wifi_ap_count,
    .wifi_scan_get_ap = plugin_api_wifi_scan_get_ap,
    .rgb_set_all = plugin_api_rgb_set_all,
};

const ghostesp_api_t *plugin_api_get(bool unsafe_allowed) {
    s_api.flags = unsafe_allowed ? GHOSTESP_APP_FLAG_UNSAFE_ALLOWED : 0;
    s_api.unsafe = unsafe_allowed ? &s_unsafe_api : NULL;
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
