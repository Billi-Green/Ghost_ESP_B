#include "gui/asset_pack.h"

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gui/screen_layout.h"
#include "managers/display_manager.h"
#include "managers/sd_card_manager.h"
#include "managers/views/main_menu_screen.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "lgfx/utility/lgfx_miniz.h"
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

#define ACTIVE_PACK_DIR "/mnt/ghostesp/themes/active"
#define ACTIVE_MANIFEST ACTIVE_PACK_DIR "/manifest.json"
#define ACTIVE_GTHEME "/mnt/ghostesp/themes/active.gtheme"
#define THEMES_DIR "/mnt/ghostesp/themes"
#define ASSET_PACK_NVS_NS "asset_pack"
#define ASSET_PACK_NVS_ACTIVE "active"
#define ASSET_PACK_MAX_ICONS 40
#define ASSET_PACK_ICON_CACHE_MAX 32
#define ASSET_PACK_ICON_CACHE_INTERNAL 2
#define ASSET_PACK_PATH_MAX 128
#define ASSET_PACK_MANIFEST_MAX (16 * 1024)
#define ASSET_PACK_ICON_MAX_RAW (128 * 128 * 3)
#define ASSET_PACK_BG_MAX_RAW (128 * 128 * 2)
#define ASSET_PACK_BG_FULLSCREEN_MAX_RAW (320 * 240 * 2)
#define ASSET_PACK_GTHEME_MAX_FILE (256 * 1024)
#define ASSET_PACK_EXTRACT_BUF_SIZE 1024
#define ASSET_PACK_GTHEME_MAX_FILES 128
#define ASSET_PACK_NAME_MAX 32

#define GIMG_FORMAT_RGB565 1
#define GIMG_FORMAT_RGB565A8 2
#define GIMG_COMP_NONE 0
#define GIMG_COMP_DEFLATE_RAW 1

static const char *TAG = "AssetPack";

typedef struct {
    char name[32];
    char small[ASSET_PACK_PATH_MAX];
    char large[ASSET_PACK_PATH_MAX];
    char xl[ASSET_PACK_PATH_MAX];
} asset_icon_entry_t;

typedef struct {
    char key[32];
    lv_img_dsc_t dsc;
    uint8_t *data;
    uint32_t last_used;
} asset_icon_cache_entry_t;

static bool s_loaded = false;
static bool s_loading_attempted = false;
static uint32_t s_version = 0;
static bool s_has_psram = false;
static bool s_has_color[6];
static uint32_t s_colors[6];
static asset_icon_entry_t s_icons[ASSET_PACK_MAX_ICONS];
static int s_icon_count = 0;
static asset_icon_cache_entry_t s_icon_cache[ASSET_PACK_ICON_CACHE_MAX];
static int s_icon_cache_size = ASSET_PACK_ICON_CACHE_INTERNAL;
static uint32_t s_cache_tick = 0;
static char s_bg_tile[ASSET_PACK_PATH_MAX];
static char s_app_icon_key[ASSET_PACK_NAME_MAX];
static char s_pack_dir[ASSET_PACK_PATH_MAX] = ACTIVE_PACK_DIR;
static char s_active_name[ASSET_PACK_NAME_MAX] = "active";
static lv_img_dsc_t s_bg_tile_dsc;
static uint8_t *s_bg_tile_data = NULL;
static lv_img_dsc_t s_bg_fullscreen_dsc;
static uint8_t *s_bg_fullscreen_data = NULL;

/* Last-resolved icon short-circuit cache. Avoids walking s_icon_cache on
 * every call when the same menu item is repeatedly queried (carousel/grid). */
static const lv_img_dsc_t *s_last_icon = NULL;
static const char *s_last_icon_key = NULL;
static uint32_t s_last_icon_version = 0;

static char s_installed_names[ASSET_PACK_INSTALLED_MAX][ASSET_PACK_NAME_MAX];
static int s_installed_count = 0;
static bool s_force_extract = false;

static esp_err_t extract_gtheme_to_active(const char *archive_path);
static void scan_installed_packs(void);

static void detect_psram_and_configure(void) {
    if (s_icon_cache_size != ASSET_PACK_ICON_CACHE_INTERNAL) return;
    size_t psram_size = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    if (psram_size > 0) {
        s_has_psram = true;
        s_icon_cache_size = ASSET_PACK_ICON_CACHE_MAX;
        ESP_LOGI(TAG, "PSRAM detected (%u bytes), icon cache=%d, background=full",
                 (unsigned)psram_size, s_icon_cache_size);
    } else {
        s_has_psram = false;
        s_icon_cache_size = ASSET_PACK_ICON_CACHE_INTERNAL;
        ESP_LOGI(TAG, "no PSRAM, icon cache=%d, background=disabled", s_icon_cache_size);
    }
}

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const uint8_t *p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

static uint64_t checksum_bytes(const uint8_t *data, size_t len) {
    uint64_t hash = 14695981039346656037ULL;
    for (size_t i = 0; i < len; ++i) {
        hash ^= data[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

static bool parse_color(const cJSON *obj, const char *key, uint32_t *out) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsString(item) || !item->valuestring) return false;
    const char *s = item->valuestring;
    if (s[0] == '#') s++;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    char *end = NULL;
    unsigned long value = strtoul(s, &end, 16);
    if (!end || *end != '\0' || value > 0xFFFFFFUL) return false;
    *out = (uint32_t)value;
    return true;
}

static bool join_pack_path(char *dst, size_t dst_len, const char *rel) {
    if (!dst || !rel || rel[0] == '\0' || rel[0] == '/' || strchr(rel, '\\') || strstr(rel, "..")) return false;
    int n = snprintf(dst, dst_len, "%s/%s", s_pack_dir, rel);
    return n > 0 && (size_t)n < dst_len;
}

static bool join_active_path(char *dst, size_t dst_len, const char *rel) {
    if (!dst || !rel || rel[0] == '\0' || rel[0] == '/' || strchr(rel, '\\') || strstr(rel, "..")) return false;
    int n = snprintf(dst, dst_len, "%s/%s", ACTIVE_PACK_DIR, rel);
    return n > 0 && (size_t)n < dst_len;
}

static bool safe_pack_id(const char *id) {
    if (!id || id[0] == '\0' || strlen(id) >= sizeof(s_active_name)) return false;
    for (const char *p = id; *p; ++p) {
        if (!(isalnum((unsigned char)*p) || *p == '_' || *p == '-')) return false;
    }
    return true;
}

static void set_pack_dir_for_name(const char *name, bool archive) {
    snprintf(s_active_name, sizeof(s_active_name), "%s", name && name[0] ? name : "active");
    if (archive) {
        snprintf(s_pack_dir, sizeof(s_pack_dir), "%s", ACTIVE_PACK_DIR);
    } else {
        snprintf(s_pack_dir, sizeof(s_pack_dir), "%s/%s", THEMES_DIR, s_active_name);
    }
}

static void save_active_name(const char *name) {
    if (!safe_pack_id(name)) return;
    nvs_handle_t nvs;
    if (nvs_open(ASSET_PACK_NVS_NS, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_str(nvs, ASSET_PACK_NVS_ACTIVE, name);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

static bool load_saved_active_name(char out[32]) {
    nvs_handle_t nvs;
    size_t len = 32;
    if (nvs_open(ASSET_PACK_NVS_NS, NVS_READONLY, &nvs) != ESP_OK) return false;
    esp_err_t err = nvs_get_str(nvs, ASSET_PACK_NVS_ACTIVE, out, &len);
    nvs_close(nvs);
    return err == ESP_OK && safe_pack_id(out);
}

static bool path_is_dir(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool path_is_file(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0 && !S_ISDIR(st.st_mode);
}

static esp_err_t asset_sd_begin(bool *mounted_here, bool *display_suspended) {
    if (mounted_here) *mounted_here = false;
    if (display_suspended) *display_suspended = false;
    if (sd_card_manager.is_initialized) return ESP_OK;
    esp_err_t err = sd_card_mount_for_flush(display_suspended);
    if (err == ESP_OK && mounted_here) *mounted_here = true;
    return err;
}

static void asset_sd_end(bool mounted_here, bool display_suspended) {
    if (mounted_here) sd_card_unmount_after_flush(display_suspended);
}

static bool pack_dir_manifest_path(const char *name, char *path, size_t path_len) {
    if (!safe_pack_id(name)) return false;
    int n = snprintf(path, path_len, "%s/%s/manifest.json", THEMES_DIR, name);
    return n > 0 && (size_t)n < path_len;
}

static bool pack_archive_path(const char *name, char *path, size_t path_len) {
    if (!safe_pack_id(name)) return false;
    int n = snprintf(path, path_len, "%s/%s.gtheme", THEMES_DIR, name);
    return n > 0 && (size_t)n < path_len;
}

static bool pack_exists(const char *name, bool *archive) {
    char path[192];
    if (pack_dir_manifest_path(name, path, sizeof(path)) && path_is_file(path)) {
        if (archive) *archive = false;
        return true;
    }
    if (pack_archive_path(name, path, sizeof(path)) && path_is_file(path)) {
        if (archive) *archive = true;
        return true;
    }
    return false;
}

static void consider_pack_candidate(const char *name, const char *current, char first[32], char next[32]) {
    if (!safe_pack_id(name)) return;
    if (first[0] == '\0' || strcmp(name, first) < 0) snprintf(first, 32, "%s", name);
    if (current && current[0] && strcmp(name, current) > 0 && (next[0] == '\0' || strcmp(name, next) < 0)) {
        snprintf(next, 32, "%s", name);
    }
}

static bool find_selectable_pack(const char *current, char out[32]) {
    out[0] = '\0';
    if (!sd_card_manager.is_initialized) {
        ESP_LOGW(TAG, "SD card is not mounted; cannot scan asset packs");
        return false;
    }
    DIR *dir = opendir(THEMES_DIR);
    if (!dir) {
        ESP_LOGW(TAG, "could not open theme directory: %s (errno=%d)", THEMES_DIR, errno);
        return false;
    }

    char first[32] = {0};
    char next[32] = {0};
    struct dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        const char *name = entry->d_name;
        if (!name || name[0] == '.') continue;

        char path[192];
        int n = snprintf(path, sizeof(path), "%s/%s", THEMES_DIR, name);
        if (n <= 0 || (size_t)n >= sizeof(path)) continue;

        if (path_is_dir(path)) {
            char manifest[192];
            if (pack_dir_manifest_path(name, manifest, sizeof(manifest)) && path_is_file(manifest)) {
                ESP_LOGI(TAG, "found asset pack folder: %s", name);
                consider_pack_candidate(name, current, first, next);
            } else if (strcmp(name, "active") != 0) {
                ESP_LOGW(TAG, "ignoring theme folder without manifest: %s", name);
            }
            continue;
        }

        if (!path_is_file(path)) continue;
        size_t len = strlen(name);
        const char *ext = ".gtheme";
        size_t ext_len = strlen(ext);
        if (len <= ext_len || strcasecmp(name + len - ext_len, ext) != 0) continue;
        if (len - ext_len >= sizeof(first)) continue;
        char id[32];
        memcpy(id, name, len - ext_len);
        id[len - ext_len] = '\0';
        ESP_LOGI(TAG, "found asset pack archive: %s", name);
        consider_pack_candidate(id, current, first, next);
    }
    closedir(dir);

    const char *chosen = next[0] ? next : first;
    if (!chosen[0]) return false;
    snprintf(out, 32, "%s", chosen);
    return true;
}

static esp_err_t select_pack_for_load(void) {
    if (!sd_card_manager.is_initialized) {
        ESP_LOGW(TAG, "cannot load asset pack: SD not mounted");
        return ESP_ERR_INVALID_STATE;
    }

    char name[32] = "active";
    char archive_path_buf[192];
    bool archive = false;

    char saved[32];
    if (load_saved_active_name(saved)) snprintf(name, sizeof(name), "%s", saved);
    if (strcmp(name, "None") == 0) {
        ESP_LOGI(TAG, "asset pack disabled (None)");
        return ESP_ERR_NOT_FOUND;
    }
    if (!pack_exists(name, &archive)) {
        if (!find_selectable_pack(NULL, name) || !pack_exists(name, &archive)) {
            ESP_LOGW(TAG, "selected asset pack not found and no fallback pack found");
            return ESP_ERR_NOT_FOUND;
        }
        save_active_name(name);
    }

    set_pack_dir_for_name(name, archive);
    ESP_LOGI(TAG, "selected asset pack '%s' (%s)", name, archive ? "archive" : "folder");
    if (!archive) return ESP_OK;
    if (!pack_archive_path(name, archive_path_buf, sizeof(archive_path_buf))) {
        ESP_LOGE(TAG, "asset pack archive path too long for '%s'", name);
        return ESP_ERR_INVALID_ARG;
    }

    char manifest_check[192];
    snprintf(manifest_check, sizeof(manifest_check), "%s/manifest.json", ACTIVE_PACK_DIR);
    if (!s_force_extract && path_is_file(manifest_check)) {
        struct stat archive_st;
        if (stat(archive_path_buf, &archive_st) == 0) {
            nvs_handle_t nvs;
            uint32_t cached_size = 0;
            uint32_t cached_mtime = 0;
            uint32_t extraction_id = 0;
            if (nvs_open(ASSET_PACK_NVS_NS, NVS_READONLY, &nvs) == ESP_OK) {
                nvs_get_u32(nvs, "archive_size", &cached_size);
                nvs_get_u32(nvs, "archive_mtime", &cached_mtime);
                nvs_get_u32(nvs, "extract_ok", &extraction_id);
                nvs_close(nvs);
            }
            if (cached_size == (uint32_t)archive_st.st_size &&
                cached_mtime == (uint32_t)archive_st.st_mtime &&
                extraction_id != 0) {
                ESP_LOGI(TAG, "archive unchanged (size=%u mtime=%u), skipping extraction",
                         (unsigned)archive_st.st_size, (unsigned)archive_st.st_mtime);
                return ESP_OK;
            }
        }
    }

    {
        nvs_handle_t nvs;
        if (nvs_open(ASSET_PACK_NVS_NS, NVS_READWRITE, &nvs) == ESP_OK) {
            nvs_set_u32(nvs, "extract_ok", 0);
            nvs_commit(nvs);
            nvs_close(nvs);
        }
    }

    esp_err_t err = extract_gtheme_to_active(archive_path_buf);
    if (err == ESP_OK) {
        struct stat archive_st;
        if (stat(archive_path_buf, &archive_st) == 0) {
            nvs_handle_t nvs;
            if (nvs_open(ASSET_PACK_NVS_NS, NVS_READWRITE, &nvs) == ESP_OK) {
                nvs_set_u32(nvs, "archive_size", (uint32_t)archive_st.st_size);
                nvs_set_u32(nvs, "archive_mtime", (uint32_t)archive_st.st_mtime);
                nvs_set_u32(nvs, "extract_ok", 1);
                nvs_commit(nvs);
                nvs_close(nvs);
            }
        }
    } else {
        ESP_LOGE(TAG, "failed to extract %s: %s", archive_path_buf, esp_err_to_name(err));
    }
    return err;
}

static bool archive_path_safe(const char *name) {
    if (!name || name[0] == '\0' || name[0] == '/' || strchr(name, '\\') || strchr(name, ':')) return false;
    if (strstr(name, "..")) return false;
    return true;
}

static int mkdir_if_missing(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) return 0;
    return mkdir(path, 0775);
}

static bool mkdir_recursive_for_dir(const char *path) {
    if (!path || path[0] == '\0') return false;
    char tmp[192];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir_if_missing(tmp) != 0 && errno != EEXIST) return false;
            *p = '/';
        }
    }
    return mkdir_if_missing(tmp) == 0 || errno == EEXIST;
}

static bool mkdir_parent_dirs(const char *path) {
    char tmp[192];
    snprintf(tmp, sizeof(tmp), "%s", path);
    char *slash = strrchr(tmp, '/');
    if (!slash) return true;
    *slash = '\0';
    return mkdir_recursive_for_dir(tmp);
}

static bool read_exact(FILE *f, void *buf, size_t len) {
    return len == 0 || fread(buf, 1, len, f) == len;
}

static bool write_bytes_to_file(const char *path, const uint8_t *data, size_t len) {
    if (!mkdir_parent_dirs(path)) return false;
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, len, f) == len;
    fclose(f);
    return ok;
}

static bool stream_archive_file(FILE *in, const char *out_path, uint32_t size, uint64_t expected_hash) {
    if (!mkdir_parent_dirs(out_path)) return false;
    FILE *out = fopen(out_path, "wb");
    if (!out) return false;

    uint8_t buf[ASSET_PACK_EXTRACT_BUF_SIZE];
    uint64_t hash = 14695981039346656037ULL;
    uint32_t remaining = size;
    bool ok = true;
    while (remaining > 0) {
        size_t want = remaining > sizeof(buf) ? sizeof(buf) : remaining;
        if (fread(buf, 1, want, in) != want) {
            ok = false;
            break;
        }
        for (size_t i = 0; i < want; ++i) {
            hash ^= buf[i];
            hash *= 1099511628211ULL;
        }
        if (fwrite(buf, 1, want, out) != want) {
            ok = false;
            break;
        }
        remaining -= (uint32_t)want;
    }
    fclose(out);
    if (!ok || hash != expected_hash) {
        unlink(out_path);
        return false;
    }
    return true;
}

static void remove_active_file(const char *rel) {
    char path[192];
    if (join_active_path(path, sizeof(path), rel)) unlink(path);
}

static void clear_active_pack_files(void) {
    remove_active_file("manifest.json");
    remove_active_file("checksums.json");
    remove_active_file("bg_tile.gimg");
}

static bool read_file_text(const char *path, char **out, size_t max_bytes) {
    *out = NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
    long size = ftell(f);
    if (size <= 0 || (size_t)size > max_bytes) { fclose(f); return false; }
    rewind(f);
    char *buf = calloc(1, (size_t)size + 1);
    if (!buf) { fclose(f); return false; }
    size_t n = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (n != (size_t)size) { free(buf); return false; }
    *out = buf;
    return true;
}

static void free_img_dsc(lv_img_dsc_t *dsc, uint8_t **data) {
    if (*data) {
        free(*data);
        *data = NULL;
    }
    memset(dsc, 0, sizeof(*dsc));
}

static void clear_runtime(void) {
    for (int i = 0; i < s_icon_cache_size; ++i) {
        free_img_dsc(&s_icon_cache[i].dsc, &s_icon_cache[i].data);
        s_icon_cache[i].key[0] = '\0';
        s_icon_cache[i].last_used = 0;
    }
    free_img_dsc(&s_bg_tile_dsc, &s_bg_tile_data);
    free_img_dsc(&s_bg_fullscreen_dsc, &s_bg_fullscreen_data);
    s_last_icon = NULL;
    s_last_icon_key = NULL;
    s_last_icon_version = 0;
    memset(s_has_color, 0, sizeof(s_has_color));
    memset(s_colors, 0, sizeof(s_colors));
    memset(s_icons, 0, sizeof(s_icons));
    memset(s_bg_tile, 0, sizeof(s_bg_tile));
    memset(s_app_icon_key, 0, sizeof(s_app_icon_key));
    s_icon_count = 0;
    s_loaded = false;
    s_version++;
}

static lv_img_cf_t gimg_cf(uint8_t fmt) {
    if (fmt == GIMG_FORMAT_RGB565A8) return LV_IMG_CF_RGB565A8;
    return LV_IMG_CF_TRUE_COLOR;
}

static esp_err_t load_gimg(const char *path, bool psram_only, uint32_t max_raw, lv_img_dsc_t *out_dsc, uint8_t **out_data) {
    *out_data = NULL;
    memset(out_dsc, 0, sizeof(*out_dsc));

    FILE *f = fopen(path, "rb");
    if (!f) return ESP_ERR_NOT_FOUND;

    uint8_t hdr[32];
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr) || memcmp(hdr, "GIMG", 4) != 0) {
        fclose(f);
        return ESP_ERR_INVALID_VERSION;
    }

    uint16_t version = rd16(hdr + 4);
    uint16_t header_size = rd16(hdr + 6);
    uint16_t width = rd16(hdr + 8);
    uint16_t height = rd16(hdr + 10);
    uint8_t fmt = hdr[12];
    uint8_t comp = hdr[13];
    uint32_t raw_size = rd32(hdr + 16);
    uint32_t payload_size = rd32(hdr + 20);
    uint64_t expected_hash = rd64(hdr + 24);

    if (version != 1 || header_size < sizeof(hdr) || width == 0 || height == 0 || raw_size == 0 || raw_size > max_raw || payload_size > max_raw) {
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }
    if (fmt != GIMG_FORMAT_RGB565 && fmt != GIMG_FORMAT_RGB565A8) {
        fclose(f);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (comp != GIMG_COMP_NONE && comp != GIMG_COMP_DEFLATE_RAW) {
        fclose(f);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (header_size > sizeof(hdr) && fseek(f, header_size, SEEK_SET) != 0) {
        fclose(f);
        return ESP_FAIL;
    }

    uint8_t *payload = heap_caps_malloc(payload_size ? payload_size : 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!payload) payload = malloc(payload_size ? payload_size : 1);
    if (!payload) { fclose(f); return ESP_ERR_NO_MEM; }

    bool read_ok = fread(payload, 1, payload_size, f) == payload_size;
    fclose(f);
    if (!read_ok) {
        free(payload);
        return ESP_FAIL;
    }

    uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    uint8_t *raw = heap_caps_malloc(raw_size, caps);
    if (!raw && !psram_only) raw = malloc(raw_size);
    if (!raw) {
        free(payload);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = ESP_OK;
    if (comp == GIMG_COMP_NONE) {
        if (payload_size != raw_size) ret = ESP_ERR_INVALID_SIZE;
        else memcpy(raw, payload, raw_size);
    } else {
        size_t out_len = lgfx_tinfl_decompress_mem_to_mem(raw, raw_size, payload, payload_size, 0);
        if (out_len == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED || out_len != raw_size) ret = ESP_FAIL;
    }
    free(payload);
    if (ret != ESP_OK) {
        free(raw);
        return ret;
    }
    if (checksum_bytes(raw, raw_size) != expected_hash) {
        free(raw);
        return ESP_ERR_INVALID_CRC;
    }

#if LV_COLOR_DEPTH == 16 && LV_COLOR_16_SWAP
    uint32_t color_bytes = raw_size;
    if (fmt == GIMG_FORMAT_RGB565A8) {
        color_bytes = (uint32_t)width * (uint32_t)height * 2;
    }
    if ((color_bytes & 1) != 0 || color_bytes > raw_size) {
        free(raw);
        return ESP_ERR_INVALID_SIZE;
    }
    for (uint32_t i = 0; i < color_bytes; i += 2) {
        uint8_t tmp = raw[i];
        raw[i] = raw[i + 1];
        raw[i + 1] = tmp;
    }
#endif

    out_dsc->header.cf = gimg_cf(fmt);
    out_dsc->header.always_zero = 0;
    out_dsc->header.reserved = 0;
    out_dsc->header.w = width;
    out_dsc->header.h = height;
    out_dsc->data_size = raw_size;
    out_dsc->data = raw;
    *out_data = raw;
    ESP_LOGD(TAG, "load_gimg OK: %s %ux%u fmt=%u comp=%u raw=%lu payload=%lu cf=%lu data=%p",
             path, width, height, fmt, comp, (unsigned long)raw_size, (unsigned long)payload_size,
             (unsigned long)out_dsc->header.cf, (void *)raw);
    return ESP_OK;
}

static esp_err_t extract_gtheme_to_active(const char *archive_path) {
    struct stat st;
    if (stat(archive_path, &st) != 0) {
        ESP_LOGE(TAG, "gtheme not found: %s (errno=%d)", archive_path, errno);
        return ESP_ERR_NOT_FOUND;
    }
    if (!mkdir_recursive_for_dir(ACTIVE_PACK_DIR)) {
        ESP_LOGE(TAG, "failed to create active theme directory: %s (errno=%d)", ACTIVE_PACK_DIR, errno);
        return ESP_FAIL;
    }

    FILE *f = fopen(archive_path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "failed to open gtheme: %s (errno=%d)", archive_path, errno);
        return ESP_FAIL;
    }

    uint8_t hdr[24];
    if (!read_exact(f, hdr, 12) || memcmp(hdr, "GAPP", 4) != 0 || rd16(hdr + 4) != 1) {
        fclose(f);
        ESP_LOGE(TAG, "invalid gtheme header: %s magic=%02X %02X %02X %02X version=%u",
                 archive_path, hdr[0], hdr[1], hdr[2], hdr[3], rd16(hdr + 4));
        if (hdr[0] == 'P' && hdr[1] == 'K') {
            ESP_LOGE(TAG, "zip-style .gtheme archives are not supported on firmware; rebuild with current gbt asset pack --archive");
        }
        return ESP_ERR_INVALID_VERSION;
    }

    uint32_t file_count = rd32(hdr + 8);
    if (file_count == 0 || file_count > ASSET_PACK_GTHEME_MAX_FILES) {
        fclose(f);
        ESP_LOGE(TAG, "invalid gtheme file count: %lu", (unsigned long)file_count);
        return ESP_ERR_INVALID_SIZE;
    }
    ESP_LOGI(TAG, "extracting gtheme %s (%lu files)", archive_path, (unsigned long)file_count);

    clear_active_pack_files();

    for (uint32_t i = 0; i < file_count; ++i) {
        if (!read_exact(f, hdr, sizeof(hdr)) || memcmp(hdr, "FILE", 4) != 0) {
            fclose(f);
            ESP_LOGE(TAG, "invalid gtheme entry header at index %lu", (unsigned long)i);
            return ESP_FAIL;
        }

        uint16_t method = rd16(hdr + 4);
        uint16_t name_len = rd16(hdr + 6);
        uint32_t raw_size = rd32(hdr + 8);
        uint32_t payload_size = rd32(hdr + 12);
        uint64_t expected_hash = rd64(hdr + 16);
        if (name_len == 0 || name_len >= ASSET_PACK_PATH_MAX || raw_size > ASSET_PACK_GTHEME_MAX_FILE || payload_size > ASSET_PACK_GTHEME_MAX_FILE) {
            fclose(f);
            ESP_LOGE(TAG, "invalid gtheme entry size at index %lu: name=%u raw=%lu payload=%lu",
                     (unsigned long)i, name_len, (unsigned long)raw_size, (unsigned long)payload_size);
            return ESP_ERR_INVALID_SIZE;
        }

        char name[ASSET_PACK_PATH_MAX];
        if (!read_exact(f, name, name_len)) {
            fclose(f);
            ESP_LOGE(TAG, "failed to read gtheme entry name at index %lu", (unsigned long)i);
            return ESP_FAIL;
        }
        name[name_len] = '\0';
        if (!archive_path_safe(name)) {
            fclose(f);
            ESP_LOGE(TAG, "unsafe gtheme entry path: %s", name);
            return ESP_ERR_INVALID_ARG;
        }

        char out_path[192];
        if (!join_active_path(out_path, sizeof(out_path), name)) {
            fclose(f);
            ESP_LOGE(TAG, "active output path too long/invalid: %s", name);
            return ESP_ERR_INVALID_ARG;
        }
        ESP_LOGD(TAG, "extracting gtheme entry %s method=%u raw=%lu payload=%lu",
                 name, method, (unsigned long)raw_size, (unsigned long)payload_size);

        esp_err_t err = ESP_OK;
        if (method == 0) {
            if (payload_size != raw_size) {
                err = ESP_ERR_INVALID_SIZE;
            } else if (!stream_archive_file(f, out_path, raw_size, expected_hash)) {
                err = ESP_FAIL;
            }
            if (err != ESP_OK) {
                fclose(f);
                ESP_LOGE(TAG, "failed to stream gtheme entry %s: %s", name, esp_err_to_name(err));
                return err;
            }
            continue;
        }

        uint8_t *payload = heap_caps_malloc(payload_size ? payload_size : 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!payload) payload = malloc(payload_size ? payload_size : 1);
        if (!payload) { fclose(f); ESP_LOGE(TAG, "no memory for gtheme payload %s", name); return ESP_ERR_NO_MEM; }
        if (!read_exact(f, payload, payload_size)) {
            free(payload);
            fclose(f);
            ESP_LOGE(TAG, "failed to read gtheme payload for %s", name);
            return ESP_FAIL;
        }

        uint8_t *raw = NULL;
        if (method == 1) {
            raw = heap_caps_malloc(raw_size ? raw_size : 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!raw) raw = malloc(raw_size ? raw_size : 1);
            if (!raw) err = ESP_ERR_NO_MEM;
            else {
                size_t out_len = lgfx_tinfl_decompress_mem_to_mem(raw, raw_size, payload, payload_size, 0);
                if (out_len == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED || out_len != raw_size) err = ESP_FAIL;
            }
        } else {
            err = ESP_ERR_NOT_SUPPORTED;
        }

        if (err == ESP_OK && checksum_bytes(raw, raw_size) != expected_hash) err = ESP_ERR_INVALID_CRC;
        if (err == ESP_OK && !write_bytes_to_file(out_path, raw, raw_size)) {
            err = ESP_FAIL;
        }

        if (raw) free(raw);
        free(payload);
        if (err != ESP_OK) {
            fclose(f);
            ESP_LOGE(TAG, "failed to extract gtheme entry %s: %s", name, esp_err_to_name(err));
            return err;
        }
    }

    fclose(f);
    ESP_LOGI(TAG, "extracted %s to active asset pack directory", archive_path);
    return ESP_OK;
}

esp_err_t asset_pack_extract_active_gtheme(void) {
    return extract_gtheme_to_active(ACTIVE_GTHEME);
}

static asset_icon_entry_t *find_icon(const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < s_icon_count; ++i) {
        if (strcmp(s_icons[i].name, name) == 0) return &s_icons[i];
    }
    return NULL;
}

static const char *pick_icon_rel(const asset_icon_entry_t *entry) {
    if (!entry) return NULL;
    bool small = LV_MIN(LV_HOR_RES, LV_VER_RES) < 200;
    if (small && entry->small[0]) return entry->small;
    if (!small && entry->large[0]) return entry->large;
    if (entry->xl[0]) return entry->xl;
    if (entry->large[0]) return entry->large;
    if (entry->small[0]) return entry->small;
    return NULL;
}

static asset_icon_cache_entry_t *cache_slot_for(const char *name) {
    if (s_icon_cache_size <= 0) return NULL;
    asset_icon_cache_entry_t *oldest = &s_icon_cache[0];
    for (int i = 0; i < s_icon_cache_size; ++i) {
        if (s_icon_cache[i].key[0] == '\0') return &s_icon_cache[i];
        if (s_icon_cache[i].last_used < oldest->last_used) oldest = &s_icon_cache[i];
    }
    free_img_dsc(&oldest->dsc, &oldest->data);
    oldest->key[0] = '\0';
    return oldest;
}

static void copy_cache_key(char dst[32], const char *src) {
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t len = strnlen(src, 31);
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static void preload_loaded_assets(void) {
    if (!s_has_psram) {
        ESP_LOGI(TAG, "asset preload skipped: no PSRAM");
        return;
    }

    if (s_bg_tile[0] && !s_bg_tile_data) {
        char path[192];
        if (join_pack_path(path, sizeof(path), s_bg_tile)) {
            esp_err_t err = load_gimg(path, true, ASSET_PACK_BG_FULLSCREEN_MAX_RAW, &s_bg_tile_dsc, &s_bg_tile_data);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "background preload failed (%s): %s", path, esp_err_to_name(err));
            }
        }
    }

    int loaded = 0;
    for (int i = 0; i < s_icon_count; ++i) {
        const char *rel = pick_icon_rel(&s_icons[i]);
        char path[192];
        if (!join_pack_path(path, sizeof(path), rel)) continue;

        asset_icon_cache_entry_t *slot = cache_slot_for(s_icons[i].name);
        if (!slot) continue;

        esp_err_t err = load_gimg(path, false, ASSET_PACK_ICON_MAX_RAW, &slot->dsc, &slot->data);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "icon preload failed for %s (%s): %s", s_icons[i].name, path, esp_err_to_name(err));
            continue;
        }
        copy_cache_key(slot->key, s_icons[i].name);
        slot->last_used = ++s_cache_tick;
        loaded++;
    }

    ESP_LOGI(TAG, "preloaded asset pack '%s': bg=%s icons=%d/%d", s_active_name,
             s_bg_tile_data ? "yes" : "no", loaded, s_icon_count);
}

static esp_err_t asset_pack_load_active_impl(void) {
    detect_psram_and_configure();
    scan_installed_packs();
    s_loading_attempted = true;
    esp_err_t select_err = select_pack_for_load();
    if (select_err != ESP_OK) {
        clear_runtime();
        return select_err;
    }

    char manifest_path[192];
    int manifest_len = snprintf(manifest_path, sizeof(manifest_path), "%s/manifest.json", s_pack_dir);
    if (manifest_len <= 0 || (size_t)manifest_len >= sizeof(manifest_path)) {
        clear_runtime();
        return ESP_ERR_INVALID_ARG;
    }

    struct stat st;
    if (stat(manifest_path, &st) != 0) {
        clear_runtime();
        return ESP_ERR_NOT_FOUND;
    }

    char *text = NULL;
    if (!read_file_text(manifest_path, &text, ASSET_PACK_MANIFEST_MAX)) {
        clear_runtime();
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(text);
    free(text);
    if (!root) {
        clear_runtime();
        ESP_LOGE(TAG, "invalid active asset pack manifest");
        return ESP_FAIL;
    }

    clear_runtime();

    const cJSON *colors = cJSON_GetObjectItemCaseSensitive(root, "colors");
    if (cJSON_IsObject(colors)) {
        static const char *keys[] = {"accent", "background", "surface", "surface_alt", "text", "text_muted"};
        for (int i = 0; i < 6; ++i) {
            s_has_color[i] = parse_color(colors, keys[i], &s_colors[i]);
        }
    }

    const cJSON *icons = cJSON_GetObjectItemCaseSensitive(root, "icons");
    if (cJSON_IsObject(icons)) {
        const cJSON *icon = NULL;
        cJSON_ArrayForEach(icon, icons) {
            if (!icon->string || !cJSON_IsObject(icon) || s_icon_count >= ASSET_PACK_MAX_ICONS) continue;
            asset_icon_entry_t *entry = &s_icons[s_icon_count];
            snprintf(entry->name, sizeof(entry->name), "%s", icon->string);
            const cJSON *small = cJSON_GetObjectItemCaseSensitive(icon, "s");
            const cJSON *large = cJSON_GetObjectItemCaseSensitive(icon, "l");
            const cJSON *xl = cJSON_GetObjectItemCaseSensitive(icon, "xl");
            if (cJSON_IsString(small) && small->valuestring) snprintf(entry->small, sizeof(entry->small), "%s", small->valuestring);
            if (cJSON_IsString(large) && large->valuestring) snprintf(entry->large, sizeof(entry->large), "%s", large->valuestring);
            if (cJSON_IsString(xl) && xl->valuestring) snprintf(entry->xl, sizeof(entry->xl), "%s", xl->valuestring);
            s_icon_count++;
        }
    }

    const cJSON *bg_tile = cJSON_GetObjectItemCaseSensitive(root, "bg_tile");
    if (cJSON_IsString(bg_tile) && bg_tile->valuestring) {
        snprintf(s_bg_tile, sizeof(s_bg_tile), "%s", bg_tile->valuestring);
    }

    const cJSON *app_icon = cJSON_GetObjectItemCaseSensitive(root, "app_icon");
    if (cJSON_IsString(app_icon) && app_icon->valuestring) {
        snprintf(s_app_icon_key, sizeof(s_app_icon_key), "%s", app_icon->valuestring);
    }

    cJSON_Delete(root);
    s_loaded = true;
    s_version++;
    preload_loaded_assets();
    ESP_LOGI(TAG, "loaded asset pack '%s': %d icon mappings (v%lu)", s_active_name, s_icon_count, (unsigned long)s_version);
    return ESP_OK;
}

esp_err_t asset_pack_load_active(void) {
    bool mounted_here = false;
    bool display_suspended = false;
    esp_err_t mount_err = asset_sd_begin(&mounted_here, &display_suspended);
    if (mount_err != ESP_OK) return ESP_ERR_INVALID_STATE;
    esp_err_t err = asset_pack_load_active_impl();
    asset_sd_end(mounted_here, display_suspended);
    return err;
}

bool asset_pack_is_loaded(void) {
    return s_loaded;
}

uint32_t asset_pack_get_version(void) {
    return s_version;
}

const char *asset_pack_active_name(void) {
    return s_active_name;
}

bool asset_pack_has_psram(void) {
    return s_has_psram;
}

int asset_pack_get_installed_count(void) {
    return s_installed_count;
}

const char *asset_pack_get_installed_name(int index) {
    if (index < 0 || index >= s_installed_count) return NULL;
    return s_installed_names[index];
}

int asset_pack_get_current_index(void) {
    if (strcmp(s_active_name, "None") == 0) return 0;
    for (int i = 1; i < s_installed_count; ++i) {
        if (strcmp(s_installed_names[i], s_active_name) == 0) return i;
    }
    return 0;
}

static void scan_installed_packs(void) {
    s_installed_count = 0;
    memset(s_installed_names, 0, sizeof(s_installed_names));

    snprintf(s_installed_names[0], ASSET_PACK_NAME_MAX, "None");
    s_installed_count = 1;

    DIR *dir = opendir(THEMES_DIR);
    if (!dir) return;

    struct dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL && s_installed_count < ASSET_PACK_INSTALLED_MAX) {
        const char *name = entry->d_name;
        if (!name || name[0] == '.') continue;
        if (strcmp(name, "active") == 0) continue;
        if (strlen(name) >= ASSET_PACK_NAME_MAX) continue;

        char path[192];
        int n = snprintf(path, sizeof(path), "%s/%s", THEMES_DIR, name);
        if (n <= 0 || (size_t)n >= sizeof(path)) continue;

        if (path_is_dir(path)) {
            char manifest[192];
            if (pack_dir_manifest_path(name, manifest, sizeof(manifest)) && path_is_file(manifest)) {
                size_t nlen = strlen(name);
                memcpy(s_installed_names[s_installed_count], name, nlen);
                s_installed_names[s_installed_count][nlen] = '\0';
                s_installed_count++;
            }
            continue;
        }

        if (!path_is_file(path)) continue;
        size_t len = strlen(name);
        const char *ext = ".gtheme";
        size_t ext_len = strlen(ext);
        if (len <= ext_len || strcasecmp(name + len - ext_len, ext) != 0) continue;
        if (len - ext_len >= ASSET_PACK_NAME_MAX) continue;
        char id[ASSET_PACK_NAME_MAX];
        memcpy(id, name, len - ext_len);
        id[len - ext_len] = '\0';
        if (safe_pack_id(id)) {
            snprintf(s_installed_names[s_installed_count], ASSET_PACK_NAME_MAX, "%s", id);
            s_installed_count++;
        }
    }
    closedir(dir);

    for (int i = 0; i < s_installed_count - 1; ++i) {
        for (int j = i + 1; j < s_installed_count; ++j) {
            if (strcmp(s_installed_names[i], s_installed_names[j]) > 0) {
                char tmp[ASSET_PACK_NAME_MAX];
                memcpy(tmp, s_installed_names[i], ASSET_PACK_NAME_MAX);
                memcpy(s_installed_names[i], s_installed_names[j], ASSET_PACK_NAME_MAX);
                memcpy(s_installed_names[j], tmp, ASSET_PACK_NAME_MAX);
            }
        }
    }

    ESP_LOGI(TAG, "found %d installed asset packs", s_installed_count);
}

esp_err_t asset_pack_select_by_index(int index) {
    if (index < 0 || index >= s_installed_count) return ESP_ERR_INVALID_ARG;

    if (index == 0) {
        save_active_name("None");
        clear_runtime();
        return ESP_OK;
    }

    bool mounted_here = false;
    bool display_suspended = false;
    esp_err_t mount_err = asset_sd_begin(&mounted_here, &display_suspended);
    if (mount_err != ESP_OK) return ESP_ERR_INVALID_STATE;

    const char *name = s_installed_names[index];
    save_active_name(name);
    bool archive = false;
    if (!pack_exists(name, &archive)) {
        asset_sd_end(mounted_here, display_suspended);
        return ESP_ERR_NOT_FOUND;
    }
    set_pack_dir_for_name(name, archive);
    s_force_extract = true;
    esp_err_t err = asset_pack_load_active_impl();
    s_force_extract = false;
    asset_sd_end(mounted_here, display_suspended);
    return err;
}

bool asset_pack_get_color(int slot, uint32_t *out_color) {
    if (!s_loaded || !out_color || slot < 0 || slot >= 6 || !s_has_color[slot]) return false;
    *out_color = s_colors[slot];
    return true;
}

const lv_img_dsc_t *asset_pack_get_icon(const char *name, const lv_img_dsc_t *fallback) {
    if (!s_loaded || !name) return fallback;

    /* Fast path: same key string pointer + same pack version -> return the
     * previously resolved desc without walking the cache. The name argument
     * is always a compile-time string literal from the menu/app item tables,
     * so pointer identity is sufficient and stable. */
    if (s_last_icon && s_last_icon_key == name && s_last_icon_version == s_version) {
        return s_last_icon;
    }

    for (int i = 0; i < s_icon_cache_size; ++i) {
        if (s_icon_cache[i].key[0] && strcmp(s_icon_cache[i].key, name) == 0 && s_icon_cache[i].data) {
            s_icon_cache[i].last_used = ++s_cache_tick;
            s_last_icon = &s_icon_cache[i].dsc;
            s_last_icon_key = name;
            s_last_icon_version = s_version;
            return &s_icon_cache[i].dsc;
        }
    }

    asset_icon_entry_t *entry = find_icon(name);
    const char *rel = pick_icon_rel(entry);
    char path[192];
    if (!join_pack_path(path, sizeof(path), rel)) {
        s_last_icon = fallback;
        s_last_icon_key = name;
        s_last_icon_version = s_version;
        return fallback;
    }

    asset_icon_cache_entry_t *slot = cache_slot_for(name);
    bool mounted_here = false;
    bool display_suspended = false;
    esp_err_t mount_err = asset_sd_begin(&mounted_here, &display_suspended);
    if (mount_err != ESP_OK) return fallback;
    esp_err_t err = load_gimg(path, false, ASSET_PACK_ICON_MAX_RAW, &slot->dsc, &slot->data);
    asset_sd_end(mounted_here, display_suspended);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "icon load failed for %s (%s): %s", name, path, esp_err_to_name(err));
        return fallback;
    }
    copy_cache_key(slot->key, name);
    slot->last_used = ++s_cache_tick;
    s_last_icon = &slot->dsc;
    s_last_icon_key = name;
    s_last_icon_version = s_version;
    return &slot->dsc;
}

const lv_img_dsc_t *asset_pack_get_app_icon(const lv_img_dsc_t *fallback) {
    if (!s_loaded || !s_app_icon_key[0]) return fallback;
    return asset_pack_get_icon(s_app_icon_key, fallback);
}

const lv_img_dsc_t *asset_pack_get_background_tile(void) {
    if (!s_loaded || !s_bg_tile[0]) return NULL;
    if (!s_has_psram) return NULL;
    if (s_bg_tile_data) return &s_bg_tile_dsc;

    char path[192];
    if (!join_pack_path(path, sizeof(path), s_bg_tile)) return NULL;
    bool mounted_here = false;
    bool display_suspended = false;
    esp_err_t mount_err = asset_sd_begin(&mounted_here, &display_suspended);
    if (mount_err != ESP_OK) return NULL;
    esp_err_t err = load_gimg(path, true, ASSET_PACK_BG_FULLSCREEN_MAX_RAW, &s_bg_tile_dsc, &s_bg_tile_data);
    asset_sd_end(mounted_here, display_suspended);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "background tile load failed: %s", esp_err_to_name(err));
        return NULL;
    }
    return &s_bg_tile_dsc;
}

/* One-time bake of the small tile into a fullscreen RGB565 PSRAM buffer.
 * Result: a single LV_IMG_CF_TRUE_COLOR desc sized to LV_HOR_RES x LV_VER_RES
 * that LVGL can blit with no per-frame tiling math. */
static esp_err_t bake_background_fullscreen(void) {
    if (s_bg_fullscreen_data) return ESP_OK;
    if (!s_bg_tile_data) return ESP_ERR_INVALID_STATE;

    int sw = s_bg_tile_dsc.header.w;
    int sh = s_bg_tile_dsc.header.h;
    int dw = LV_HOR_RES;
    int dh = LV_VER_RES;
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return ESP_ERR_INVALID_SIZE;

    size_t size = (size_t)dw * (size_t)dh * 2;
    if (size > ASSET_PACK_BG_FULLSCREEN_MAX_RAW) return ESP_ERR_INVALID_SIZE;

    uint8_t *buf = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) return ESP_ERR_NO_MEM;

    const uint8_t *src = s_bg_tile_data;
    for (int y = 0; y < dh; y++) {
        int src_y = y % sh;
        uint8_t *dst_row = buf + (size_t)y * dw * 2;
        const uint8_t *src_row = src + (size_t)src_y * sw * 2;
        int x = 0;
        while (x + sw <= dw) {
            memcpy(dst_row + x * 2, src_row, (size_t)sw * 2);
            x += sw;
        }
        int rem = dw - x;
        if (rem > 0) memcpy(dst_row + x * 2, src_row, (size_t)rem * 2);
    }

    s_bg_fullscreen_dsc = s_bg_tile_dsc;
    s_bg_fullscreen_dsc.header.w = (uint16_t)dw;
    s_bg_fullscreen_dsc.header.h = (uint16_t)dh;
    s_bg_fullscreen_dsc.data_size = (uint32_t)size;
    s_bg_fullscreen_dsc.data = buf;
    s_bg_fullscreen_data = buf;
    ESP_LOGI(TAG, "baked fullscreen bg: %dx%d (%u bytes) from %dx%d tile",
             dw, dh, (unsigned)size, sw, sh);
    return ESP_OK;
}

const lv_img_dsc_t *asset_pack_get_background_fullscreen(void) {
    if (!s_loaded || !s_has_psram) return NULL;
    if (s_bg_fullscreen_data) return &s_bg_fullscreen_dsc;
    if (!asset_pack_get_background_tile()) return NULL;
    if (bake_background_fullscreen() != ESP_OK) {
        ESP_LOGW(TAG, "fullscreen bg bake failed; callers will fall back to tile");
        return NULL;
    }
    return &s_bg_fullscreen_dsc;
}

typedef struct {
    int index;
} switch_task_args_t;

static void switch_pack_task(void *arg) {
    switch_task_args_t *args = (switch_task_args_t *)arg;
    int index = args->index;
    free(args);

    ESP_LOGI(TAG, "switch_pack_task: loading pack index %d", index);
    esp_err_t err = asset_pack_select_by_index(index);
    ESP_LOGI(TAG, "switch_pack_task: result=%s", esp_err_to_name(err));

    if (err == ESP_OK) {
        display_manager_update_status_bar_color();
        gui_screen_apply_background(main_menu_view.root);
        ESP_LOGI(TAG, "switch_pack_task: UI refresh scheduled via version counter");
    } else if (err != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "asset pack switch failed: %s", esp_err_to_name(err));
    }

    vTaskDelete(NULL);
}

void asset_pack_switch_task(int index) {
    switch_task_args_t *args = malloc(sizeof(switch_task_args_t));
    if (!args) return;
    args->index = index;
    xTaskCreate(switch_pack_task, "pack_switch", 12288, args, 6, NULL);
}
