#include "managers/plugin_manager.h"

#include "managers/plugin_api.h"
#include "managers/sd_card_manager.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_err.h"
#include "sdkconfig.h"
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define PLUGIN_APPS_DIR "/mnt/ghostesp/apps"
#define PLUGIN_MANIFEST_MAX_BYTES 8192

static const char *TAG = "PluginManager";
static plugin_app_manifest_t *s_apps = NULL;
static int s_app_count = 0;
static char s_last_error[128];

static void set_error(const char *fmt, const char *arg) {
    snprintf(s_last_error, sizeof(s_last_error), fmt, arg ? arg : "");
}

static bool is_safe_id(const char *id) {
    if (!id || id[0] == '\0') return false;
    for (const char *p = id; *p; ++p) {
        if (!(isalnum((unsigned char)*p) || *p == '_' || *p == '-')) return false;
    }
    return true;
}

static bool join_path(char *dst, size_t dst_len, const char *base, const char *name) {
    if (!dst || dst_len == 0 || !base || !name) return false;
    size_t base_len = strlen(base);
    size_t name_len = strlen(name);
    if (base_len == 0 || name_len == 0 || base_len + 1 + name_len + 1 > dst_len) return false;
    memcpy(dst, base, base_len);
    dst[base_len] = '/';
    memcpy(dst + base_len + 1, name, name_len);
    dst[base_len + 1 + name_len] = '\0';
    return true;
}

static void copy_json_string(cJSON *root, const char *key, char *dst, size_t dst_len) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsString(item) && item->valuestring) {
        strncpy(dst, item->valuestring, dst_len - 1);
        dst[dst_len - 1] = '\0';
    }
}

static uint32_t permission_from_string(const char *value) {
    if (!value) return 0;
    if (strcmp(value, "ui") == 0) return PLUGIN_PERMISSION_UI;
    if (strcmp(value, "storage") == 0) return PLUGIN_PERMISSION_STORAGE;
    if (strcmp(value, "commands") == 0 || strcmp(value, "command") == 0) return PLUGIN_PERMISSION_COMMANDS;
    if (strcmp(value, "tasks") == 0) return PLUGIN_PERMISSION_TASKS;
    if (strcmp(value, "wifi") == 0) return PLUGIN_PERMISSION_WIFI;
    if (strcmp(value, "ble") == 0) return PLUGIN_PERMISSION_BLE;
    if (strcmp(value, "nfc") == 0) return PLUGIN_PERMISSION_NFC;
    if (strcmp(value, "ir") == 0 || strcmp(value, "infrared") == 0) return PLUGIN_PERMISSION_IR;
    if (strcmp(value, "subghz") == 0) return PLUGIN_PERMISSION_SUBGHZ;
    if (strcmp(value, "badusb") == 0) return PLUGIN_PERMISSION_BADUSB;
    if (strcmp(value, "raw_gpio") == 0) return PLUGIN_PERMISSION_RAW_GPIO;
    if (strcmp(value, "lvgl") == 0) return PLUGIN_PERMISSION_LVGL;
    return 0;
}

static bool read_file_to_buffer(const char *path, char **out_buf) {
    *out_buf = NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
    long size = ftell(f);
    if (size <= 0 || size > PLUGIN_MANIFEST_MAX_BYTES) { fclose(f); return false; }
    rewind(f);
    char *buf = calloc(1, (size_t)size + 1);
    if (!buf) { fclose(f); return false; }
    size_t n = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (n != (size_t)size) { free(buf); return false; }
    *out_buf = buf;
    return true;
}

static bool parse_manifest(const char *base_path, plugin_app_manifest_t *out) {
    char manifest_path[PLUGIN_APP_PATH_MAX];
    if (!join_path(manifest_path, sizeof(manifest_path), base_path, "manifest.json")) {
        snprintf(out->error, sizeof(out->error), "manifest path too long");
        return false;
    }

    char *buf = NULL;
    if (!read_file_to_buffer(manifest_path, &buf)) {
        snprintf(out->error, sizeof(out->error), "manifest read failed");
        return false;
    }

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        snprintf(out->error, sizeof(out->error), "manifest json invalid");
        return false;
    }

    copy_json_string(root, "id", out->id, sizeof(out->id));
    copy_json_string(root, "name", out->name, sizeof(out->name));
    copy_json_string(root, "version", out->version, sizeof(out->version));
    copy_json_string(root, "author", out->author, sizeof(out->author));
    copy_json_string(root, "target", out->target, sizeof(out->target));
    copy_json_string(root, "entry", out->entry, sizeof(out->entry));

    cJSON *api_version = cJSON_GetObjectItemCaseSensitive(root, "api_version");
    out->api_version = cJSON_IsNumber(api_version) ? (uint32_t)api_version->valueint : 0;
    cJSON *unsafe = cJSON_GetObjectItemCaseSensitive(root, "unsafe");
    out->unsafe = cJSON_IsBool(unsafe) && cJSON_IsTrue(unsafe);

    cJSON *permissions = cJSON_GetObjectItemCaseSensitive(root, "permissions");
    if (cJSON_IsArray(permissions)) {
        cJSON *perm = NULL;
        cJSON_ArrayForEach(perm, permissions) {
            if (cJSON_IsString(perm)) out->permissions |= permission_from_string(perm->valuestring);
        }
    }

    cJSON_Delete(root);

    strncpy(out->base_path, base_path, sizeof(out->base_path) - 1);
    if (!join_path(out->entry_path, sizeof(out->entry_path), base_path, out->entry)) {
        snprintf(out->error, sizeof(out->error), "entry path too long");
        return false;
    }

    if (!is_safe_id(out->id)) {
        snprintf(out->error, sizeof(out->error), "invalid id");
        return false;
    }
    if (out->name[0] == '\0') strncpy(out->name, out->id, sizeof(out->name) - 1);
    if (out->entry[0] == '\0') {
        snprintf(out->error, sizeof(out->error), "missing entry");
        return false;
    }
    if (strstr(out->entry, "..") || strchr(out->entry, '/') || strchr(out->entry, '\\')) {
        snprintf(out->error, sizeof(out->error), "invalid entry path");
        return false;
    }
    if (out->api_version != GHOSTESP_APP_API_VERSION) {
        snprintf(out->error, sizeof(out->error), "api version mismatch");
        return false;
    }
    if (!sd_card_exists(out->entry_path)) {
        snprintf(out->error, sizeof(out->error), "entry missing");
        return false;
    }

    out->valid = true;
    return true;
}

void plugin_manager_init(void) {
    if (!s_apps) {
        s_apps = calloc(PLUGIN_APP_MAX_COUNT, sizeof(*s_apps));
        if (!s_apps) {
            snprintf(s_last_error, sizeof(s_last_error), "failed to allocate app registry");
            return;
        }
    }
    s_last_error[0] = '\0';
}

bool plugin_manager_target_supported(void) {
#if CONFIG_ENABLE_NATIVE_SD_APPS
    return true;
#else
    return false;
#endif
}

bool plugin_manager_target_matches(const plugin_app_manifest_t *app) {
    if (!app) return false;
    if (app->target[0] == '\0') return true;
    return strcmp(app->target, plugin_api_current_target()) == 0;
}

int plugin_manager_reload(void) {
    plugin_manager_init();
    if (!s_apps) return -1;
    s_app_count = 0;
    memset(s_apps, 0, sizeof(*s_apps) * PLUGIN_APP_MAX_COUNT);
    s_last_error[0] = '\0';

    bool display_was_suspended = false;
    bool mounted_here = false;
    if (!sd_card_manager.is_initialized) {
        if (sd_card_mount_for_flush(&display_was_suspended) != ESP_OK) {
            set_error("failed to mount SD for %s", "apps");
            return -1;
        }
        mounted_here = true;
    }

    DIR *dir = opendir(PLUGIN_APPS_DIR);
    if (!dir) {
        set_error("failed to open %s", PLUGIN_APPS_DIR);
        if (mounted_here) sd_card_unmount_after_flush(display_was_suspended);
        return -1;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && s_app_count < PLUGIN_APP_MAX_COUNT) {
        if (entry->d_name[0] == '.') continue;
        char base_path[PLUGIN_APP_PATH_MAX];
        if (!join_path(base_path, sizeof(base_path), PLUGIN_APPS_DIR, entry->d_name)) {
            ESP_LOGW(TAG, "Skipping app with too-long path: %s", entry->d_name);
            continue;
        }

        struct stat st;
        if (stat(base_path, &st) != 0 || !S_ISDIR(st.st_mode)) continue;

        plugin_app_manifest_t app = {0};
        if (!parse_manifest(base_path, &app)) {
            ESP_LOGW(TAG, "Skipping app at %s: %s", base_path, app.error);
            continue;
        }
        s_apps[s_app_count++] = app;
    }

    closedir(dir);
    if (mounted_here) sd_card_unmount_after_flush(display_was_suspended);
    ESP_LOGI(TAG, "Loaded %d SD app manifests", s_app_count);
    return s_app_count;
}

int plugin_manager_count(void) {
    return s_app_count;
}

const plugin_app_manifest_t *plugin_manager_get(int index) {
    if (!s_apps) return NULL;
    if (index < 0 || index >= s_app_count) return NULL;
    return &s_apps[index];
}

const plugin_app_manifest_t *plugin_manager_find(const char *id) {
    if (!s_apps || !id) return NULL;
    for (int i = 0; i < s_app_count; ++i) {
        if (strcmp(s_apps[i].id, id) == 0) return &s_apps[i];
    }
    return NULL;
}

const char *plugin_manager_last_error(void) {
    return s_last_error;
}
