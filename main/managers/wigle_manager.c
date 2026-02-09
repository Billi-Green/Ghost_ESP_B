/**
 * Wigle.net upload manager.
 * Uploads wardriving CSV files (sweeps, GPS logs) to Wigle via API.
 * API: https://api.wigle.net/api/v2/file/upload
 * Auth: Basic (APIName:APIToken from wigle.net/account)
 */

#include "managers/wigle_manager.h"
#include "managers/settings_manager.h"
#include "core/glog.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_crt_bundle.h"
#include "mbedtls/base64.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <stdint.h>
#include "freertos/task.h"
#include "esp_netif.h"

#define WIGLE_UPLOAD_URL "https://api.wigle.net/api/v2/file/upload"
#define WIGLE_MAX_FILE_SIZE (32 * 1024)
#define WIGLE_BOUNDARY "------------------------GhostESP"
#define WIGLE_UPLOADED_FILE "/mnt/ghostesp/.wigle_uploaded"
#define WIGLE_RECENT_MAX 16
#define AUTH_BUF_SIZE 256
#define WIGLE_RESP_BUF_SIZE 384
#define WIGLE_STREAM_CHUNK 512
#define WIGLE_TASK_STACK 8192

typedef struct {
    char buf[WIGLE_RESP_BUF_SIZE];
    size_t len;
} wigle_resp_t;

static esp_err_t wigle_http_event(esp_http_client_event_t *evt) {
    wigle_resp_t *r = (wigle_resp_t *)evt->user_data;
    if (!r || evt->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
    size_t copy = evt->data_len;
    if (r->len + copy >= WIGLE_RESP_BUF_SIZE) copy = WIGLE_RESP_BUF_SIZE - r->len - 1;
    if (copy > 0) {
        memcpy(r->buf + r->len, evt->data, copy);
        r->len += copy;
        r->buf[r->len] = '\0';
    }
    return ESP_OK;
}

void wigle_set_api_key(const char *key) {
    if (!key) return;
    FSettings *s = &G_Settings;
    strncpy(s->wigle_api_key, key, sizeof(s->wigle_api_key) - 1);
    s->wigle_api_key[sizeof(s->wigle_api_key) - 1] = '\0';
    settings_save(s);
}

const char *wigle_get_api_key(void) {
    return G_Settings.wigle_api_key;
}

/**
 * Upload a single file to Wigle.
 * Builds multipart body: file + donate=on.
 * Only uploads files with WigleWifi pre-header (skips sweep CSV).
 */
static bool wigle_file_is_valid_format(FILE *f) {
    char line[80];
    if (!fgets(line, sizeof(line), f)) return false;
    return (strstr(line, "WigleWifi") != NULL);
}

/* Check STA has IP; return true if ready for upload */
static bool wigle_sta_has_ip(void) {
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!sta) return false;
    esp_netif_ip_info_t ip = {0};
    return (esp_netif_get_ip_info(sta, &ip) == ESP_OK && ip.ip.addr != 0);
}

/* Check if file (basename,size) was already uploaded. File format: basename,size\n */
static bool wigle_uploaded_check(const char *basename, long size) {
    FILE *f = fopen(WIGLE_UPLOADED_FILE, "r");
    if (!f) return false;
    char line[160];
    char expect[160];
    snprintf(expect, sizeof(expect), "%s,%ld", basename, size);
    bool found = false;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (strcmp(line, expect) == 0) {
            found = true;
            break;
        }
    }
    fclose(f);
    return found;
}

/* Append basename,size to uploaded list after successful upload */
static void wigle_uploaded_add(const char *basename, long size) {
    FILE *f = fopen(WIGLE_UPLOADED_FILE, "a");
    if (!f) {
        glog("Wigle: cannot append to %s\n", WIGLE_UPLOADED_FILE);
        return;
    }
    fprintf(f, "%s,%ld\n", basename, size);
    fclose(f);
}

/* Wigle rejects files with only pre-header + header (no data rows) */
static bool wigle_file_has_data_rows(FILE *f) {
    long pos = ftell(f);
    fseek(f, 0, SEEK_SET);
    int newlines = 0;
    int c;
    while ((c = fgetc(f)) != EOF && newlines < 3) {
        if (c == '\n') newlines++;
    }
    fseek(f, pos, SEEK_SET);
    return newlines >= 2;  /* pre-header, header, and at least one data row */
}

static esp_err_t wigle_upload_file(const char *filepath, const char *api_key) {
    FILE *f = fopen(filepath, "rb");
    if (!f) {
        glog("Wigle: cannot open %s\n", filepath);
        return ESP_FAIL;
    }

    if (!wigle_file_is_valid_format(f)) {
        fclose(f);
        glog("Wigle: skip %s (not Wigle CSV format)\n", filepath);
        return ESP_ERR_NOT_SUPPORTED;
    }
    fseek(f, 0, SEEK_SET);
    if (!wigle_file_has_data_rows(f)) {
        fclose(f);
        glog("Wigle: skip %s (no data rows - need GPS fix + wardriving)\n", filepath);
        return ESP_ERR_NOT_SUPPORTED;
    }
    fseek(f, 0, SEEK_SET);

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize <= 0 || fsize > (long)WIGLE_MAX_FILE_SIZE) {
        fclose(f);
        glog("Wigle: file %s size %ld out of range (max %d)\n",
             filepath, fsize, WIGLE_MAX_FILE_SIZE);
        return ESP_ERR_INVALID_SIZE;
    }

    size_t file_len = (size_t)fsize;

    const char *basename = strrchr(filepath, '/');
    if (!basename) basename = filepath;
    else basename++;

    if (wigle_uploaded_check(basename, fsize)) {
        fclose(f);
        glog("Wigle: skip %s (already uploaded)\n", filepath);
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* Multipart body size:
     * boundary + headers + file + boundary + donate + closing
     */
    size_t part1_len = strlen("--" WIGLE_BOUNDARY "\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"\"\r\n"
        "Content-Type: text/csv\r\n\r\n");
    size_t part1_fname = strlen(basename);
    part1_len += part1_fname;

    size_t part2_len = strlen("\r\n--" WIGLE_BOUNDARY "\r\n"
        "Content-Disposition: form-data; name=\"donate\"\r\n\r\n"
        "true\r\n"
        "--" WIGLE_BOUNDARY "--\r\n");

    size_t body_len = part1_len + file_len + part2_len;

    /* Allocate from PSRAM first; fallback to heap */
    char *body = heap_caps_malloc(body_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    bool use_caps = (body != NULL);
    if (!body) body = (char *)malloc(body_len);
    if (!body) {
        fclose(f);
        glog("Wigle: OOM for upload body\n");
        return ESP_ERR_NO_MEM;
    }

    char *p = body;
    p += snprintf(p, (size_t)(body + body_len - p), "--" WIGLE_BOUNDARY "\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"%s\"\r\n"
        "Content-Type: text/csv\r\n\r\n", basename);
    size_t read_len = fread(p, 1, file_len, f);
    fclose(f);
    if (read_len != file_len) {
        if (use_caps) heap_caps_free(body);
        else free(body);
        return ESP_FAIL;
    }
    p += read_len;
    memcpy(p, "\r\n--" WIGLE_BOUNDARY "\r\n"
        "Content-Disposition: form-data; name=\"donate\"\r\n\r\n"
        "true\r\n"
        "--" WIGLE_BOUNDARY "--\r\n", part2_len);

    /* Build Authorization: Basic base64(APIName:APIToken) */
    char auth_b64[AUTH_BUF_SIZE];
    size_t enc_len = AUTH_BUF_SIZE - 1;
    int r = mbedtls_base64_encode((unsigned char *)auth_b64, AUTH_BUF_SIZE, &enc_len,
                                  (const unsigned char *)api_key, strlen(api_key));
    if (r != 0) {
        if (use_caps) heap_caps_free(body);
        else free(body);
        glog("Wigle: base64 encode failed\n");
        return ESP_FAIL;
    }
    auth_b64[enc_len] = '\0';

    char auth_val[6 + AUTH_BUF_SIZE + 2];
    snprintf(auth_val, sizeof(auth_val), "Basic %s", auth_b64);

    char content_type_hdr[128];
    snprintf(content_type_hdr, sizeof(content_type_hdr),
             "multipart/form-data; boundary=%s", WIGLE_BOUNDARY);

    wigle_resp_t resp = {0};

    esp_http_client_config_t config = {
        .url = WIGLE_UPLOAD_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 90000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .event_handler = wigle_http_event,
        .user_data = &resp,
        .buffer_size = 4096,
        .buffer_size_tx = 4096,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        if (use_caps) heap_caps_free(body);
        else free(body);
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "User-Agent", "GhostESP/1.0");
    esp_http_client_set_header(client, "Authorization", auth_val);
    esp_http_client_set_header(client, "Content-Type", content_type_hdr);
    esp_http_client_set_post_field(client, body, (int)body_len);

    esp_err_t err = esp_http_client_perform(client);
    if (use_caps) heap_caps_free(body);
    else free(body);

    int status = esp_http_client_get_status_code(client);
    if (err != ESP_OK) {
        int tls_code = 0, tls_flags = 0;
        if (esp_http_client_get_and_clear_last_tls_error(client, &tls_code, &tls_flags) == ESP_OK && (tls_code != 0 || tls_flags != 0)) {
            glog("Wigle: TLS err 0x%x flags 0x%x\n", (unsigned)tls_code, (unsigned)tls_flags);
        }
    }
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        glog("Wigle: HTTP err 0x%x %s\n", (unsigned)err, esp_err_to_name(err));
        return err;
    }
    if (status < 200 || status >= 300) {
        glog("Wigle: upload %s failed HTTP %d\n", filepath, status);
        if (resp.len > 0) {
            resp.buf[WIGLE_RESP_BUF_SIZE - 1] = '\0';
            glog("Wigle response: %s\n", resp.buf);
        }
        return ESP_FAIL;
    }
    glog("Wigle: uploaded %s\n", filepath);
    wigle_uploaded_add(basename, fsize);
    return ESP_OK;
}

typedef struct {
    char path[320];
    time_t mtime;
    long size;
} wigle_file_entry_t;

/* Collect eligible Wigle CSVs from dir into entries, return count. */
static int wigle_collect_from_dir(const char *dirpath, wigle_file_entry_t *entries, int max_count) {
    DIR *d = opendir(dirpath);
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while (n < max_count && (e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        size_t len = strlen(e->d_name);
        int is_csv = (len > 4 && strcasecmp(e->d_name + len - 4, ".csv") == 0) ||
                     (len > 9 && strcasecmp(e->d_name + len - 9, ".wiglecsv") == 0);
        if (!is_csv) continue;

        char path[320];
        (void)snprintf(path, sizeof(path), "%s/%s", dirpath, e->d_name);

        struct stat st;
        if (stat(path, &st) != 0) continue;

        FILE *f = fopen(path, "rb");
        if (!f) continue;
        if (!wigle_file_is_valid_format(f)) { fclose(f); continue; }
        fseek(f, 0, SEEK_SET);
        if (!wigle_file_has_data_rows(f)) { fclose(f); continue; }
        fseek(f, 0, SEEK_END);
        long fsize = ftell(f);
        fclose(f);

        if (fsize <= 0 || fsize > (long)WIGLE_MAX_FILE_SIZE) continue;

        snprintf(entries[n].path, sizeof(entries[n].path), "%s", path);
        entries[n].mtime = st.st_mtime;
        entries[n].size = fsize;
        n++;
    }
    closedir(d);
    return n;
}

/* Upload the N most recent Wigle CSVs (by mtime). Skips already-uploaded. */
static esp_err_t wigle_upload_recent_impl(int count, const char *api_key) {
    size_t entries_sz = sizeof(wigle_file_entry_t) * WIGLE_RECENT_MAX;
    wigle_file_entry_t *entries = heap_caps_malloc(entries_sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    bool use_caps = (entries != NULL);
    if (!entries) entries = malloc(entries_sz);
    if (!entries) {
        glog("Wigle: OOM for entries\n");
        return ESP_ERR_NO_MEM;
    }
    int total = wigle_collect_from_dir("/mnt/ghostesp/sweeps", entries, WIGLE_RECENT_MAX);
    if (total < WIGLE_RECENT_MAX) {
        total += wigle_collect_from_dir("/mnt/ghostesp/gps", entries + total, WIGLE_RECENT_MAX - total);
    }
    if (total == 0) {
        glog("Wigle: no eligible CSV files found\n");
        if (use_caps) heap_caps_free(entries);
        else free(entries);
        return ESP_OK;
    }
    /* Sort by mtime descending (newest first) */
    for (int i = 0; i < total - 1; i++) {
        for (int j = i + 1; j < total; j++) {
            if (entries[j].mtime > entries[i].mtime) {
                wigle_file_entry_t tmp = entries[i];
                entries[i] = entries[j];
                entries[j] = tmp;
            }
        }
    }
    int uploaded = 0, failed = 0;
    for (int i = 0; i < total && uploaded < count; i++) {
        const char *basename = strrchr(entries[i].path, '/');
        if (!basename) basename = entries[i].path;
        else basename++;
        if (wigle_uploaded_check(basename, entries[i].size)) continue;
        esp_err_t ret = wigle_upload_file(entries[i].path, api_key);
        if (ret == ESP_OK) uploaded++;
        else if (ret != ESP_ERR_NOT_SUPPORTED) failed++;
    }
    if (use_caps) heap_caps_free(entries);
    else free(entries);
    glog("Wigle: done. uploaded=%d failed=%d\n", uploaded, failed);
    return (failed > 0) ? ESP_FAIL : ESP_OK;
}

/**
 * Iterate directory and upload .csv and .wiglecsv files.
 */
static esp_err_t wigle_upload_from_dir(const char *dirpath, const char *api_key,
                                       int *uploaded, int *failed) {
    DIR *d = opendir(dirpath);
    if (!d) return ESP_OK;

    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        size_t len = strlen(e->d_name);
        int is_csv = (len > 4 && strcasecmp(e->d_name + len - 4, ".csv") == 0) ||
                     (len > 9 && strcasecmp(e->d_name + len - 9, ".wiglecsv") == 0);
        if (!is_csv) continue;

        char path[320];
        (void)snprintf(path, sizeof(path), "%s/%s", dirpath, e->d_name);

        esp_err_t ret = wigle_upload_file(path, api_key);
        if (ret == ESP_OK) (*uploaded)++;
        else if (ret != ESP_ERR_NOT_SUPPORTED) (*failed)++;
    }
    closedir(d);
    return ESP_OK;
}

esp_err_t wigle_upload_all(void) {
    const char *api_key = wigle_get_api_key();
    if (!api_key || api_key[0] == '\0') {
        glog("Wigle: no API key set. Use 'wigle API <name>:<token>'\n");
        return ESP_ERR_INVALID_STATE;
    }

    if (strchr(api_key, ':') == NULL) {
        glog("Wigle: API key must be format APIName:APIToken\n");
        return ESP_ERR_INVALID_ARG;
    }

    if (!wigle_sta_has_ip()) {
        glog("Wigle: STA not connected - connect to WiFi first (Menu > WiFi > Connect)\n");
        return ESP_ERR_INVALID_STATE;
    }

    glog("Wigle: starting upload...\n");

    int uploaded = 0, failed = 0;

    wigle_upload_from_dir("/mnt/ghostesp/sweeps", api_key, &uploaded, &failed);
    wigle_upload_from_dir("/mnt/ghostesp/gps", api_key, &uploaded, &failed);

    glog("Wigle: done. uploaded=%d failed=%d\n", uploaded, failed);

    return (failed > 0) ? ESP_FAIL : ESP_OK;
}

esp_err_t wigle_upload_recent(int count) {
    const char *api_key = wigle_get_api_key();
    if (!api_key || api_key[0] == '\0') {
        glog("Wigle: no API key set. Use 'wigle API <name>:<token>'\n");
        return ESP_ERR_INVALID_STATE;
    }
    if (strchr(api_key, ':') == NULL) {
        glog("Wigle: API key must be format APIName:APIToken\n");
        return ESP_ERR_INVALID_ARG;
    }
    if (!wigle_sta_has_ip()) {
        glog("Wigle: STA not connected - connect to WiFi first\n");
        return ESP_ERR_INVALID_STATE;
    }
    if (count <= 0) return ESP_OK;
    glog("Wigle: uploading %d most recent CSV(s)...\n", count);
    return wigle_upload_recent_impl(count, api_key);
}

void wigle_uploaded_list(void) {
    FILE *f = fopen(WIGLE_UPLOADED_FILE, "r");
    if (!f) {
        glog("Wigle: no upload history (%s missing)\n", WIGLE_UPLOADED_FILE);
        return;
    }
    glog("Wigle: uploaded CSV memory (%s):\n", WIGLE_UPLOADED_FILE);
    char line[160];
    int n = 0;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0]) {
            glog("  %d: %s\n", ++n, line);
        }
    }
    fclose(f);
    if (n == 0) glog("  (empty)\n");
}

static void wigle_upload_all_task(void *arg) {
    (void)arg;
    wigle_upload_all();
    vTaskDelete(NULL);
}

static void wigle_upload_recent_task(void *arg) {
    int count = (int)(intptr_t)arg;
    wigle_upload_recent(count);
    vTaskDelete(NULL);
}

void wigle_upload_all_async(void) {
    const char *key = wigle_get_api_key();
    if (!key || key[0] == '\0' || strchr(key, ':') == NULL) return;
    if (xTaskCreate(wigle_upload_all_task, "wigle_up", WIGLE_TASK_STACK, NULL, 5, NULL) == pdPASS) {
        glog("Wigle: auto-upload started\n");
    } else {
        glog("Wigle: failed to start upload task\n");
    }
}

void wigle_upload_recent_async(int count) {
    const char *key = wigle_get_api_key();
    if (!key || key[0] == '\0' || strchr(key, ':') == NULL) return;
    if (count <= 0) return;
    void *arg = (void *)(intptr_t)count;
    if (xTaskCreate(wigle_upload_recent_task, "wigle_up", WIGLE_TASK_STACK, arg, 5, NULL) == pdPASS) {
        glog("Wigle: uploading %d most recent\n", count);
    } else {
        glog("Wigle: failed to start upload task\n");
    }
}

