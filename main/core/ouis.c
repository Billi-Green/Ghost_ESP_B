#include "core/ouis.h"
#include <esp_http_server.h>
#include <esp_log.h>
#include <string.h>
#include <ctype.h>

extern const uint8_t ouis_json_start[] asm("_binary_ouis_json_start");
extern const uint8_t ouis_json_end[]   asm("_binary_ouis_json_end");

static void normalize_prefix(const char *mac, char *out6) {
    int oi = 0;
    for (const char *p = mac; *p && oi < 6; ++p) {
        if ((*p >= '0' && *p <= '9') || (*p >= 'A' && *p <= 'F') || (*p >= 'a' && *p <= 'f')) {
            out6[oi++] = (char)toupper((unsigned char)*p);
        }
    }
    while (oi < 6) out6[oi++] = '\0';
    out6[6] = '\0';
}

static bool is_locally_administered(const char *mac) {
    // check laa bit: second least significant bit of first octet
    int hi = 0, lo = 0;
    if (!isxdigit((int)mac[0]) || !isxdigit((int)mac[1])) return false;
    hi = (mac[0] <= '9' ? mac[0]-'0' : toupper(mac[0])-'A'+10);
    lo = (mac[1] <= '9' ? mac[1]-'0' : toupper(mac[1])-'A'+10);
    int first_octet = (hi<<4) | lo;
    return (first_octet & 0x02) != 0;
}

static void to_proper_caps(char *s) {
    // properly capitalise: first letter upper, rest lower for each token split by space or hyphen
    bool new_word = true;
    for (size_t i = 0; s[i]; ++i) {
        if (s[i] == ' ' || s[i] == '-' || s[i] == '_' ) { new_word = true; continue; }
        if (new_word) { s[i] = (char)toupper((unsigned char)s[i]); new_word = false; }
        else { s[i] = (char)tolower((unsigned char)s[i]); }
    }
}

// naive streaming scan of embedded json: {"FC3497":"espressif",...}
static bool lookup_vendor(const char *prefix6, char *out_vendor, size_t out_sz) {
    const char *buf = (const char*)ouis_json_start;
    const char *end = (const char*)ouis_json_end;
    const size_t len = (size_t)(end - buf);
    if (len == 0) return false;
    // search for "PREFIX"
    char keypat[16];
    // pattern like "FC3497"
    snprintf(keypat, sizeof(keypat), "\"%s\"", prefix6);
    const char *p = buf;
    while (p && p < end) {
        const char *k = strstr(p, keypat);
        if (!k || k >= end) break;
        const char *colon = strchr(k + strlen(keypat), ':');
        if (!colon || colon >= end) break;
        const char *q1 = strchr(colon + 1, '"');
        if (!q1 || q1 >= end) break;
        const char *q2 = strchr(q1 + 1, '"');
        if (!q2 || q2 >= end) break;
        size_t vlen = (size_t)(q2 - (q1 + 1));
        if (vlen >= out_sz) vlen = out_sz - 1;
        memcpy(out_vendor, q1 + 1, vlen);
        out_vendor[vlen] = '\0';
        to_proper_caps(out_vendor);
        return true;
    
        // move forward
        p = k + 1;
    }
    return false;
}

bool ouis_lookup_vendor(const char *mac, char *out_vendor, size_t out_sz) {
    char p6[8] = {0};
    normalize_prefix(mac, p6);
    if (strlen(p6) < 6 || is_locally_administered(p6)) {
        return false;
    }
    return lookup_vendor(p6, out_vendor, out_sz);
}

static esp_err_t handle_oui_lookup(httpd_req_t *req) {
    char mac[64] = {0};
    // prefer post json body, fall back to query string for compatibility
    if (req->content_len > 0 && req->content_len < 256) {
        char *body = (char*)malloc(req->content_len + 1);
        if (body) {
            int r = httpd_req_recv(req, body, req->content_len);
            if (r > 0) body[r] = '\0'; else body[0] = '\0';
            char *p = strstr(body, "\"mac\"");
            if (p) {
                p = strchr(p, ':');
                if (p) {
                    p++;
                    while (*p==' '||*p=='\t'||*p=='\"') p++;
                    char *q = p;
                    while (*q && *q!='\"' && (q - body) < req->content_len) q++;
                    size_t l = (size_t)(q - p);
                    if (l > sizeof(mac)-1) l = sizeof(mac)-1;
                    memcpy(mac, p, l); mac[l] = '\0';
                }
            }
            free(body);
        }
    }
    if (mac[0] == '\0' && httpd_req_get_url_query_len(req) > 0) {
        char q[128];
        if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
            char val[64];
            if (httpd_query_key_value(q, "mac", val, sizeof(val)) == ESP_OK) {
                strncpy(mac, val, sizeof(mac)-1);
            }
        }
    }
    if (mac[0] == '\0') {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"success\":false,\"message\":\"Mac Required\"}", -1);
    }

    char vendor[64] = {0};
    bool ok = ouis_lookup_vendor(mac, vendor, sizeof(vendor));
    httpd_resp_set_type(req, "application/json");
    if (ok) {
        char resp[128];
        snprintf(resp, sizeof(resp), "{\"success\":true,\"vendor\":\"%s\"}", vendor);
        return httpd_resp_send(req, resp, -1);
    } else {
        return httpd_resp_send(req, "{\"success\":false,\"vendor\":null}", -1);
    }
}

void ouis_register_handlers(httpd_handle_t handle) {
    static httpd_uri_t uri_oui_post = {.uri = "/oui", .method = HTTP_POST, .handler = handle_oui_lookup, .user_ctx = NULL};
    static httpd_uri_t uri_oui_get  = {.uri = "/oui", .method = HTTP_GET,  .handler = handle_oui_lookup, .user_ctx = NULL};
    httpd_register_uri_handler(handle, &uri_oui_post);
    httpd_register_uri_handler(handle, &uri_oui_get);
    ESP_LOGI("OUIS", "Registered /oui Endpoint (GET, POST)");
}
