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
#define WIGLE_BOUNDARY "------------------------GhostESP"
#define WIGLE_UPLOADED_FILE "/mnt/ghostesp/.wigle_uploaded"
#define WIGLE_QUEUE_FILE "/mnt/ghostesp/.wigle_queue"
/* Max queue entries processed per connect; kept small to limit RAM. */
#define WIGLE_QUEUE_BATCH_MAX 16
#define WIGLE_STREAM_CHUNK_SIZE 4096
#define AUTH_BUF_SIZE 256
#define WIGLE_RESP_BUF_SIZE 384
#define WIGLE_TASK_STACK 16384

static volatile bool wigle_upload_in_progress = false;

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

/**
 * Upload a single CSV file to WiGLE API.
 * 
 * Multipart structure:
 *   --------------------------GhostESP\r\n
 *   Content-Disposition: form-data; name="file"; filename="test.csv"\r\n
 *   Content-Type: text/csv\r\n\r\n
 *   [file content]
 *   \r\n--------------------------GhostESP\r\n
 *   Content-Disposition: form-data; name="donate"\r\n\r\n
 *   true\r\n
 *   --------------------------GhostESP--\r\n
 * 
 * Content-Type header: multipart/form-data; boundary=------------------------GhostESP
 * Note: boundary in header has NO dashes, body boundaries have "--" prefix
 */
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

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize <= 0) {
        fclose(f);
        glog("Wigle: file %s size %ld invalid\n", filepath, fsize);
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

    const char *donate_val = G_Settings.wigle_donate ? "true" : "false";
    size_t part2_len = strlen("\r\n--" WIGLE_BOUNDARY "\r\n"
        "Content-Disposition: form-data; name=\"donate\"\r\n\r\n") +
        strlen(donate_val) + strlen("\r\n--" WIGLE_BOUNDARY "--\r\n");

    size_t body_len = part1_len + file_len + part2_len;

    /* Build Authorization: Basic base64(APIName:APIToken) */
    char auth_b64[AUTH_BUF_SIZE];
    size_t enc_len = AUTH_BUF_SIZE - 1;
    int r = mbedtls_base64_encode((unsigned char *)auth_b64, AUTH_BUF_SIZE, &enc_len,
                                  (const unsigned char *)api_key, strlen(api_key));
    if (r != 0) {
        fclose(f);
        glog("Wigle: base64 encode failed\n");
        return ESP_FAIL;
    }
    auth_b64[enc_len] = '\0';

    char auth_val[6 + AUTH_BUF_SIZE + 1];
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
        fclose(f);
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "User-Agent", "GhostESP/1.0");
    esp_http_client_set_header(client, "Authorization", auth_val);
    esp_http_client_set_header(client, "Content-Type", content_type_hdr);

    esp_err_t err = esp_http_client_open(client, (int)body_len);
    if (err != ESP_OK) {
        fclose(f);
        esp_http_client_cleanup(client);
        return err;
    }

    /* Write part1 (boundary + headers) */
    char part1[256];
    int p1_len = snprintf(part1, sizeof(part1), "--" WIGLE_BOUNDARY "\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"%s\"\r\n"
        "Content-Type: text/csv\r\n\r\n", basename);
    int written = esp_http_client_write(client, part1, p1_len);
    if (written != p1_len) {
        fclose(f);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    /* Stream file content (heap buffer to keep task stack small) */
    char *chunk = (char *)malloc(WIGLE_STREAM_CHUNK_SIZE);
    if (!chunk) {
        fclose(f);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }
    size_t remaining = file_len;
    while (remaining > 0) {
        size_t to_read = (remaining > WIGLE_STREAM_CHUNK_SIZE) ? WIGLE_STREAM_CHUNK_SIZE : remaining;
        size_t read_len = fread(chunk, 1, to_read, f);
        if (read_len == 0) break;
        written = esp_http_client_write(client, chunk, (int)read_len);
        if (written != (int)read_len) {
            free(chunk);
            fclose(f);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }
        remaining -= read_len;
    }
    free(chunk);
    fclose(f);

    /* Write part2 (boundary + donate + closing) */
    char part2[160];
    int p2_len = snprintf(part2, sizeof(part2), "\r\n--" WIGLE_BOUNDARY "\r\n"
        "Content-Disposition: form-data; name=\"donate\"\r\n\r\n"
        "%s\r\n"
        "--" WIGLE_BOUNDARY "--\r\n", donate_val);
    written = esp_http_client_write(client, part2, p2_len);
    if (written != p2_len) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    if (err != ESP_OK) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return err;
    }

    /* After writing body manually, fetch response headers */
    int content_len = esp_http_client_fetch_headers(client);
    if (content_len < 0) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return (esp_err_t)content_len;
    }

    /* Drain response body to complete the request */
    char resp_drain[128];
    while (esp_http_client_read(client, resp_drain, sizeof(resp_drain)) > 0) {
        /* Response captured by event handler */
    }

    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (status < 200 || status >= 300) {
        glog("Wigle: upload %s failed HTTP %d\n", filepath, status);
        return ESP_FAIL;
    }
    glog("Wigle: uploaded %s\n", filepath);
    wigle_uploaded_add(basename, fsize);
    return ESP_OK;
}

/* Add file path to upload queue (only if not already uploaded) */
void wigle_queue_add(const char *filepath) {
    const char *basename = strrchr(filepath, '/');
    if (!basename) basename = filepath;
    else basename++;
    
    struct stat st;
    if (stat(filepath, &st) != 0) return;
    
    /* Skip if already uploaded */
    if (wigle_uploaded_check(basename, st.st_size)) return;
    
    FILE *q = fopen(WIGLE_QUEUE_FILE, "a");
    if (!q) return;
    fprintf(q, "%s\n", filepath);
    fclose(q);
}

/* Process queue file: upload queued files, remove successful ones from queue.
 * Reads queue line-by-line to avoid large buffers. Only files that fail to
 * upload are written back to the queue for retry. */
static esp_err_t wigle_process_queue(const char *api_key) {
    FILE *q = fopen(WIGLE_QUEUE_FILE, "r");
    if (!q) {
        /* No queue yet: scan GPS directory and enqueue valid Wigle CSVs. */
        char *scan_path = malloc(320);
        if (!scan_path) {
            return ESP_ERR_NO_MEM;
        }
        DIR *d = opendir("/mnt/ghostesp/gps");
        if (d) {
            struct dirent *e;
            while ((e = readdir(d)) != NULL) {
                if (e->d_name[0] == '.') continue;
                size_t len = strlen(e->d_name);
                if (len <= 4 || strcasecmp(e->d_name + len - 4, ".csv") != 0) continue;
                (void)snprintf(scan_path, 320, "/mnt/ghostesp/gps/%s", e->d_name);
                FILE *f = fopen(scan_path, "rb");
                if (f) {
                    if (wigle_file_is_valid_format(f)) {
                        wigle_queue_add(scan_path);
                    }
                    fclose(f);
                }
            }
            closedir(d);
        }
        free(scan_path);
        q = fopen(WIGLE_QUEUE_FILE, "r");
        if (!q) {
            glog("Wigle: files already uploaded, no new files to upload\n");
            return ESP_ERR_NOT_FOUND;
        }
    }

    FILE *q_out = fopen(WIGLE_QUEUE_FILE ".tmp", "w");
    if (!q_out) {
        fclose(q);
        return ESP_FAIL;
    }

    char *line = malloc(320);
    if (!line) {
        fclose(q);
        fclose(q_out);
        return ESP_ERR_NO_MEM;
    }

    int uploaded = 0, failed = 0, skipped = 0;

    while (fgets(line, 320, q)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (!line[0]) continue;

        const char *path = line;
        const char *basename = strrchr(path, '/');
        if (!basename) basename = path;
        else basename++;

        struct stat st;
        if (stat(path, &st) != 0) {
            /* File doesn't exist, drop from queue */
            continue;
        }

        /* Skip if already uploaded (remove from queue) */
        if (wigle_uploaded_check(basename, st.st_size)) {
            skipped++;
            continue;
        }

        esp_err_t ret = wigle_upload_file(path, api_key);
        if (ret == ESP_OK) {
            uploaded++;
            /* Successfully uploaded, don't add back to queue */
        } else if (ret == ESP_ERR_NOT_SUPPORTED) {
            /* Invalid/empty Wigle CSV, drop from queue */
            continue;
        } else {
            failed++;
            /* Failed upload, keep in queue for retry */
            fprintf(q_out, "%s\n", path);
        }
    }

    free(line);
    fclose(q);
    fclose(q_out);

    if (failed > 0) {
        /* Keep only failures in the queue. */
        remove(WIGLE_QUEUE_FILE);
        if (rename(WIGLE_QUEUE_FILE ".tmp", WIGLE_QUEUE_FILE) != 0) {
            /* If rename fails, leave tmp as-is for inspection. */
        }
    } else {
        /* No failures: no queue needed. */
        remove(WIGLE_QUEUE_FILE);
        remove(WIGLE_QUEUE_FILE ".tmp");
    }

    if (uploaded > 0 && failed == 0) {
        glog("Wigle: all files uploaded successfully (%d)\n", uploaded);
        return ESP_OK;
    } else if (uploaded > 0 && failed > 0) {
        glog("Wigle: uploaded=%d failed=%d\n", uploaded, failed);
        return ESP_FAIL;
    } else if (failed > 0) {
        glog("Wigle: upload failed (%d files)\n", failed);
        return ESP_FAIL;
    } else {
        glog("Wigle: files already uploaded, no new files to upload\n");
        return ESP_ERR_NOT_FOUND;
    }
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

    return wigle_process_queue(api_key);
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
    (void)wigle_upload_all();
    wigle_upload_in_progress = false;
    vTaskDelete(NULL);
}

void wigle_upload_all_async(void) {
    if (wigle_upload_in_progress) {
        glog("Wigle: upload already in progress\n");
        return;
    }
    const char *key = wigle_get_api_key();
    if (!key || key[0] == '\0' || strchr(key, ':') == NULL) return;
    wigle_upload_in_progress = true;
    if (xTaskCreate(wigle_upload_all_task, "wigle_up", WIGLE_TASK_STACK, NULL, 5, NULL) == pdPASS) {
        glog("Wigle: auto-upload started\n");
    } else {
        glog("Wigle: failed to start upload task\n");
        wigle_upload_in_progress = false;
    }
}

