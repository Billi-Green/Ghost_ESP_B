// new nfc_view.c - simple NFC view based on options_screen layout
#include "managers/display_manager.h"
#include "managers/views/main_menu_screen.h"
#include "managers/settings_manager.h"
#include "lvgl.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdarg.h>
#include "managers/sd_card_manager.h"
#ifdef CONFIG_HAS_NFC
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pn532.h"
#include "driver/i2c.h"
#include "pn532_driver.h"
#include "pn532_driver_i2c.h"
#endif

// Forward declaration of this view instance for internal references
extern View nfc_view;

static const char *TAG = "NFCView";

// touch nav button sizing to match options_screen
#define SCROLL_BTN_SIZE 40
#define SCROLL_BTN_PADDING 5

static lv_style_t style_menu_item;
static lv_style_t style_menu_item_alt;
static lv_style_t style_selected_item;
static lv_style_t style_menu_label;
static bool styles_initialized = false;

static lv_obj_t *root = NULL;
static lv_obj_t *menu_container = NULL;
static lv_obj_t *scan_btn = NULL;
static lv_obj_t *emulate_btn = NULL;
static lv_obj_t *scroll_up_btn = NULL;
static lv_obj_t *scroll_down_btn = NULL;
static lv_obj_t *back_btn = NULL;
static int selected_index = 0;
static int num_items = 0; // will be set when building menu

// Match options_screen theme palettes for selected row color
static const uint32_t theme_palettes[15][6] = {
    {0x1976D2,0xD32F2F,0x388E3C,0x7B1FA2,0x000000,0xFF9800},
    {0xFFCDD2,0xC8E6C9,0xB3E5FC,0xFFF9C4,0xD1C4E9,0xCFD8DC},
    {0x263238,0x37474F,0x455A64,0x546E7A,0x263238,0x37474F},
    {0xFFFFFF,0xFFFFFF,0xFFFFFF,0xFFFFFF,0xFFFFFF,0xFFFFFF},
    {0x002B36,0x073642,0x586E75,0x839496,0xEEE8D5,0x002B36},
    {0x888888,0x888888,0x888888,0x888888,0x888888,0x888888},
    {0xE91E63,0xE91E63,0xE91E63,0xE91E63,0xE91E63,0xE91E63},
    {0x9C27B0,0x9C27B0,0x9C27B0,0x9C27B0,0x9C27B0,0x9C27B0},
    {0x2196F3,0x2196F3,0x2196F3,0x2196F3,0x2196F3,0x2196F3},
    {0xFFA500,0xFFA500,0xFFA500,0xFFA500,0xFFA500,0xFFA500},
    {0x39FF14,0xFF073A,0x0FF1CE,0xF8F32B,0xFF6EC7,0xFF8C00},
    {0xFF00FF,0x00FFFF,0xFF0000,0x00FF00,0xFFFF00,0x800080},
    {0x0077BE,0x00CED1,0x20B2AA,0x4682B4,0x5F9EA0,0x00008B},
    {0xFF4500,0xFF8C00,0xFFD700,0xFF1493,0x8B008B,0x2E0854},
    {0x556B2F,0x6B8E23,0x228B22,0x2E8B57,0x8FBC8F,0x8B4513}
};

static int button_height_global = 0;
static bool is_small_screen_global = false;

// NFC scan popup (modeled after IR learning popup)
static lv_obj_t *nfc_scan_popup = NULL;
static lv_obj_t *nfc_scan_cancel_btn = NULL;
static lv_obj_t *nfc_scan_more_btn = NULL;
static lv_obj_t *nfc_scan_save_btn = NULL;
static lv_obj_t *nfc_title_label = NULL;
static lv_obj_t *nfc_uid_label = NULL;
static lv_obj_t *nfc_type_label = NULL;
static lv_obj_t *nfc_details_label = NULL;
static int nfc_popup_selected = 0; // 0 = Cancel, 1 = More (when available)
static bool nfc_more_visible = false;
static bool nfc_details_visible = false;
static bool nfc_save_visible = false;
static void nfc_scan_cancel_cb(lv_event_t *e);
static void nfc_scan_more_cb(lv_event_t *e);
static void nfc_scan_save_cb(lv_event_t *e);
static void create_nfc_scan_popup(void);
static void cleanup_nfc_scan_popup(void *obj);
static void update_nfc_popup_selection(void);
static void update_nfc_buttons_layout(void);
static void nfc_show_details_view(bool show);
static void write_flipper_nfc_file(void);

#ifdef CONFIG_HAS_NFC
static pn532_io_handle_t g_pn532 = NULL;
static pn532_io_t g_pn532_instance;
static TaskHandle_t nfc_scan_task_handle = NULL;
static char *nfc_details_text = NULL;
static bool nfc_details_ready = false;
static uint8_t g_uid[10] = {0};
static uint8_t g_uid_len = 0;
static uint16_t g_atqa = 0;
static uint8_t g_sak = 0;
static NTAG2XX_MODEL g_model = NTAG2XX_UNKNOWN;

// URI prefix table for NDEF URI records
static const char* ndef_uri_prefix[] = {
    [0x00] = NULL,
    [0x01] = "http://www.",
    [0x02] = "https://www.",
    [0x03] = "http://",
    [0x04] = "https://",
    [0x05] = "tel:",
    [0x06] = "mailto:",
    [0x07] = "ftp://anonymous:anonymous@",
    [0x08] = "ftp://ftp.",
    [0x09] = "ftps://",
    [0x0A] = "sftp://",
    [0x0B] = "smb://",
    [0x0C] = "nfs://",
    [0x0D] = "ftp://",
    [0x0E] = "dav://",
    [0x0F] = "news:",
    [0x10] = "telnet://",
    [0x11] = "imap:",
    [0x12] = "rtsp://",
    [0x13] = "urn:",
    [0x14] = "pop:",
    [0x15] = "sip:",
    [0x16] = "sips:",
    [0x17] = "tftp:",
    [0x18] = "btspp://",
    [0x19] = "btl2cap://",
    [0x1A] = "btgoep://",
    [0x1B] = "tcpobex://",
    [0x1C] = "irdaobex://",
    [0x1D] = "file://",
    [0x1E] = "urn:epc:id:",
    [0x1F] = "urn:epc:tag:",
    [0x20] = "urn:epc:pat:",
    [0x21] = "urn:epc:raw:",
    [0x22] = "urn:epc:",
    [0x23] = "urn:nfc:",
};

typedef struct {
    char *text;      // allocated details text
    size_t text_len; // length of text
} ndef_details_result_t;

static void nfc_set_details_async(void *ptr) {
    if (!ptr) return;
    if (!nfc_scan_popup || !lv_obj_is_valid(nfc_scan_popup)) {
        ndef_details_result_t *res = (ndef_details_result_t *)ptr;
        if (res->text) free(res->text);
        free(res);
        return;
    }
    // Replace old details if any
    if (nfc_details_text) { free(nfc_details_text); nfc_details_text = NULL; }
    ndef_details_result_t *res = (ndef_details_result_t *)ptr;
    nfc_details_text = res->text;
    nfc_details_ready = true;
    // If already showing details, update label
    if (nfc_details_visible && nfc_details_label && lv_obj_is_valid(nfc_details_label)) {
        lv_label_set_text(nfc_details_label, nfc_details_text);
    }
    free(res);
}

// Helpers to read NTAG user memory and parse NDEF TLV/records
static bool ntag_read_user_memory(pn532_io_handle_t io, uint8_t **out_buf, size_t *out_len, NTAG2XX_MODEL *out_model) {
    if (!io || !out_buf || !out_len) return false;
    *out_buf = NULL; *out_len = 0; if (out_model) *out_model = NTAG2XX_UNKNOWN;
    uint8_t page0_3[16] = {0};
    if (ntag2xx_read_page(io, 0, page0_3, sizeof(page0_3)) != ESP_OK) return false;
    // Capability Container at page 3
    if (page0_3[12] != 0xE1) {
        // Not a CC magic, still allow but mark unknown
    }
    uint8_t size_mul_8 = page0_3[14];
    size_t data_bytes = size_mul_8 * 8; // user memory size
    if (data_bytes == 0 || data_bytes > 1024) data_bytes = 1024; // safety cap
    if (out_model) {
        switch (size_mul_8) {
            case 0x12: *out_model = NTAG2XX_NTAG213; break;
            case 0x3E: *out_model = NTAG2XX_NTAG215; break;
            case 0x6D: *out_model = NTAG2XX_NTAG216; break;
            default: *out_model = NTAG2XX_UNKNOWN; break;
        }
    }
    uint8_t *buf = (uint8_t *)malloc(data_bytes);
    if (!buf) return false;
    size_t copied = 0;
    // Read starting at page 4
    while (copied < data_bytes) {
        uint8_t page = 4 + (copied / 4);
        uint8_t tmp[16] = {0};
        if (ntag2xx_read_page(io, page, tmp, sizeof(tmp)) != ESP_OK) { free(buf); return false; }
        size_t remain = data_bytes - copied;
        size_t to_copy = (remain < sizeof(tmp)) ? remain : sizeof(tmp);
        memcpy(buf + copied, tmp, to_copy);
        copied += to_copy;
        // Stop if we see TLV Terminator 0xFE and we are past it
        for (size_t i = 0; i + 1 < to_copy; ++i) {
            if (tmp[i] == 0xFE) { *out_buf = buf; *out_len = copied - (to_copy - i - 1); return true; }
        }
    }
    *out_buf = buf;
    *out_len = copied;
    return true;
}

static const char* ntag_model_str(NTAG2XX_MODEL m) {
    switch(m) {
        case NTAG2XX_NTAG213: return "NTAG213";
        case NTAG2XX_NTAG215: return "NTAG215";
        case NTAG2XX_NTAG216: return "NTAG216";
        default: return "NTAG2xx";
    }
}

static size_t append_str(char **p, size_t *cap, const char *s) {
    size_t l = strlen(s);
    if (l + 1 > *cap) return 0;
    memcpy(*p, s, l); *p += l; *cap -= l; **p = '\0'; return l;
}

static size_t append_fmt(char **p, size_t *cap, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(*p, *cap, fmt, ap);
    va_end(ap);
    if (n <= 0) return 0;
    size_t used = (size_t)n;
    if (used >= *cap) { used = *cap ? *cap - 1 : 0; }
    *p += used; *cap -= used; return used;
}

// Parse NDEF payloads (URI, Text, SmartPoster subset)
static void parse_ndef_record(uint8_t tnf, const uint8_t *type, uint8_t type_len, const uint8_t *payload, size_t payload_len, char **out, size_t *cap) {
    if (tnf == 0x01 && type_len == 1 && type[0] == 'U' && payload_len >= 1) {
        uint8_t code = payload[0];
        const char *pre = (code < (sizeof(ndef_uri_prefix)/sizeof(ndef_uri_prefix[0])) ? ndef_uri_prefix[code] : NULL);
        if (pre && strcmp(pre, "tel:") == 0) append_str(out, cap, "Phone: ");
        else if (pre && strcmp(pre, "mailto:") == 0) append_str(out, cap, "Mail: ");
        else append_str(out, cap, "URL: ");
        if (pre) append_str(out, cap, pre);
        // Rest of payload as-is
        for (size_t i = 1; i < payload_len && *cap > 1; ++i) {
            unsigned char c = payload[i];
            if (c >= 32 && c <= 126) { **out = (char)c; (*out)++; (*cap)--; }
            else { append_fmt(out, cap, "%%%02X", c); }
        }
        append_str(out, cap, "\n");
        return;
    }
    if (tnf == 0x01 && type_len == 1 && type[0] == 'T' && payload_len >= 1) {
        uint8_t status = payload[0];
        uint8_t lang_len = status & 0x3F;
        size_t text_off = 1 + lang_len;
        if (text_off > payload_len) text_off = payload_len;
        append_str(out, cap, "Text: ");
        for (size_t i = text_off; i < payload_len && *cap > 1; ++i) {
            unsigned char c = payload[i];
            if (c >= 32 && c <= 126) { **out = (char)c; (*out)++; (*cap)--; }
        }
        append_str(out, cap, "\n");
        return;
    }
    if (tnf == 0x01 && type_len == 2 && type[0] == 'S' && type[1] == 'p' && payload_len > 0) {
        // SmartPoster: payload is an embedded NDEF message
        size_t pos = 0;
        while (pos < payload_len) {
            if (pos + 1 > payload_len) break;
            uint8_t flags = payload[pos++];
            uint8_t tlen = (pos < payload_len) ? payload[pos++] : 0;
            uint32_t plen = 0;
            if (flags & 0x10) { // SR
                plen = (pos < payload_len) ? payload[pos++] : 0;
            } else {
                if (pos + 4 > payload_len) break;
                plen = ((uint32_t)payload[pos] << 24) | ((uint32_t)payload[pos+1] << 16) | ((uint32_t)payload[pos+2] << 8) | payload[pos+3];
                pos += 4;
            }
            uint8_t idlen = (flags & 0x08) ? ((pos < payload_len) ? payload[pos++] : 0) : 0;
            const uint8_t *tt = (pos + tlen <= payload_len) ? &payload[pos] : NULL; pos += tlen;
            const uint8_t *id = (pos + idlen <= payload_len) ? &payload[pos] : NULL; (void)id; pos += idlen;
            const uint8_t *pl = (pos + plen <= payload_len) ? &payload[pos] : NULL; pos += plen;
            if (!tt || !pl) break;
            parse_ndef_record(flags & 0x07, tt, tlen, pl, plen, out, cap);
            if (flags & 0x40) break; // ME
        }
        return;
    }
    // Fallback: print basic info
    append_str(out, cap, "Record: ");
    append_fmt(out, cap, "tnf=0x%02X type=", tnf);
    for (uint8_t i = 0; i < type_len; ++i) { if (*cap > 1) { **out = (char)type[i]; (*out)++; (*cap)--; } }
    append_str(out, cap, "\n");
}

static char* build_ndef_details_from_t2(uint8_t *mem, size_t mem_len, const uint8_t *uid, uint8_t uid_len, NTAG2XX_MODEL model) {
    // Find NDEF TLV 0x03
    size_t pos = 0; size_t end = mem_len;
    size_t msg_off = 0; size_t msg_len = 0;
    while (pos < end) {
        uint8_t tlv = mem[pos++];
        if (tlv == 0x00) continue; // padding
        if (tlv == 0xFE) break; // terminator
        uint32_t len = 0;
        if (pos >= end) break;
        if (mem[pos] != 0xFF) { len = mem[pos++]; }
        else { // 3-byte length
            if (pos + 3 > end) break;
            pos++;
            len = ((uint32_t)mem[pos] << 8) | mem[pos+1];
            pos += 2;
        }
        if (tlv == 0x03) { msg_off = pos; msg_len = len; break; }
        pos += len;
    }

    size_t cap = 1024;
    char *out = (char*)malloc(cap);
    if (!out) return NULL;
    char *w = out; *w = '\0';

    append_fmt(&w, &cap, "Card: %s\n", ntag_model_str(model));
    append_str(&w, &cap, "UID:");
    for (uint8_t i = 0; i < uid_len; ++i) append_fmt(&w, &cap, " %02X", uid[i]);
    append_str(&w, &cap, "\n");

    if (msg_len == 0 || msg_off + msg_len > end) {
        append_str(&w, &cap, "No NDEF message found\n");
        return out;
    }

    append_fmt(&w, &cap, "NDEF message: %u bytes\n", (unsigned)msg_len);
    size_t mpos = msg_off; size_t mend = msg_off + msg_len; int rec_idx = 0;
    while (mpos < mend) {
        if (mpos + 1 > mend) break;
        uint8_t flags = mem[mpos++];
        uint8_t tlen = (mpos < mend) ? mem[mpos++] : 0;
        uint32_t plen = 0;
        if (flags & 0x10) { // SR
            plen = (mpos < mend) ? mem[mpos++] : 0;
        } else {
            if (mpos + 4 > mend) break;
            plen = ((uint32_t)mem[mpos] << 24) | ((uint32_t)mem[mpos+1] << 16) | ((uint32_t)mem[mpos+2] << 8) | mem[mpos+3];
            mpos += 4;
        }
        uint8_t idlen = (flags & 0x08) ? ((mpos < mend) ? mem[mpos++] : 0) : 0;
        const uint8_t *type = (mpos + tlen <= mend) ? &mem[mpos] : NULL; mpos += tlen;
        const uint8_t *id = (mpos + idlen <= mend) ? &mem[mpos] : NULL; (void)id; mpos += idlen;
        const uint8_t *pl = (mpos + plen <= mend) ? &mem[mpos] : NULL; mpos += plen;
        if (!type || !pl) break;
        append_fmt(&w, &cap, "R%d: ", ++rec_idx);
        parse_ndef_record(flags & 0x07, type, tlen, pl, plen, &w, &cap);
        if (flags & 0x40) break; // ME
    }
    return out;
}

static void nfc_build_and_set_details(pn532_io_handle_t io, const uint8_t *uid, uint8_t uid_len) {
    uint8_t *mem = NULL; size_t mem_len = 0; NTAG2XX_MODEL model = NTAG2XX_UNKNOWN;
    if (!ntag_read_user_memory(io, &mem, &mem_len, &model)) {
        // Still provide basic details
        size_t cap = 256;
        ndef_details_result_t *res = (ndef_details_result_t*)malloc(sizeof(*res));
        if (!res) {
            return;
        }
        res->text = (char*)malloc(cap);
        res->text_len = cap;
        if (!res->text) { free(res); return; }
        char *w = res->text; snprintf(w, cap, "UID:"); size_t used = strlen(w); w += used; cap -= used;
        for (uint8_t i = 0; i < uid_len && cap > 3; ++i) { int n = snprintf(w, cap, " %02X", uid[i]); if (n>0){ w+=n; cap-=n; } }
        snprintf(w, cap, "\nNo NDEF data\n");
        lv_async_call(nfc_set_details_async, res);
        return;
    }
    char *text = build_ndef_details_from_t2(mem, mem_len, uid, uid_len, model);
    free(mem);
    if (!text) return;
    g_model = model;
    ndef_details_result_t *res = (ndef_details_result_t*)malloc(sizeof(*res));
    if (!res) { free(text); return; }
    res->text = text; res->text_len = strlen(text);
    lv_async_call(nfc_set_details_async, res);
}

static void nfc_update_labels_async(void *uid_buf) {
    if (!nfc_scan_popup || !lv_obj_is_valid(nfc_scan_popup)) { free(uid_buf); return; }
    uint8_t *uid = (uint8_t *)uid_buf;
    char uid_text[64];
    int pos = 0;
    pos += snprintf(uid_text, sizeof(uid_text), "UID:");
    for (int i = 0; i < uid[0] && pos < (int)sizeof(uid_text) - 4; ++i) {
        pos += snprintf(uid_text + pos, sizeof(uid_text) - pos, " %02X", uid[i + 1]);
    }
    lv_label_set_text(nfc_uid_label, uid_text);
    lv_label_set_text(nfc_type_label, "Type: ISO14443A");
    if (!nfc_details_visible && nfc_title_label && lv_obj_is_valid(nfc_title_label)) {
        lv_label_set_text(nfc_title_label, "NFC Tag");
        lv_obj_align(nfc_title_label, LV_ALIGN_TOP_MID, 0, 22);
    }
    // Reveal buttons once a tag has been scanned
    if (nfc_scan_more_btn && lv_obj_is_valid(nfc_scan_more_btn) && !nfc_more_visible) {
        lv_obj_clear_flag(nfc_scan_more_btn, LV_OBJ_FLAG_HIDDEN);
        nfc_more_visible = true;
    }
    if (nfc_scan_save_btn && lv_obj_is_valid(nfc_scan_save_btn) && !nfc_save_visible) {
        lv_obj_clear_flag(nfc_scan_save_btn, LV_OBJ_FLAG_HIDDEN);
        nfc_save_visible = true;
    }
    update_nfc_buttons_layout();
    update_nfc_popup_selection();
    free(uid);
}

static void nfc_scan_task(void *arg) {
    const char *TAGT = "NFCScan";
    if (g_pn532 == NULL) {
        g_pn532 = &g_pn532_instance;
        if (pn532_new_driver_i2c(
                (gpio_num_t)CONFIG_NFC_SDA_PIN,
                (gpio_num_t)CONFIG_NFC_SCL_PIN,
                (gpio_num_t)CONFIG_NFC_RST_PIN,
                (gpio_num_t)CONFIG_NFC_IRQ_PIN,
                I2C_NUM_0,
                g_pn532) != ESP_OK) {
            ESP_LOGE(TAGT, "pn532_new_driver_i2c failed");
            nfc_scan_task_handle = NULL;
            vTaskDelete(NULL);
        }
        if (pn532_init(g_pn532) != ESP_OK) {
            ESP_LOGE(TAGT, "pn532_init failed");
            nfc_scan_task_handle = NULL;
            vTaskDelete(NULL);
        }
        pn532_set_passive_activation_retries(g_pn532, 0xFF);
    }

    while (nfc_scan_popup && lv_obj_is_valid(nfc_scan_popup)) {
        uint8_t uid[8] = {0};
        uint8_t uid_len = 0;
        uint16_t atqa = 0; uint8_t sak = 0;
        esp_err_t r = pn532_read_passive_target_id_ex(g_pn532, 0x00, uid + 1, &uid_len, &atqa, &sak, 500);
        if (r == ESP_OK && uid_len > 0 && uid_len <= 7) {
            uid[0] = uid_len;
            uint8_t *copy = (uint8_t *)malloc(uid_len + 1);
            if (copy) {
                memcpy(copy, uid, uid_len + 1);
                lv_async_call(nfc_update_labels_async, copy);
            }
            // Build detailed info synchronously before exiting the task
            g_uid_len = uid_len; memcpy(g_uid, uid + 1, uid_len); g_atqa = atqa; g_sak = sak; g_model = NTAG2XX_UNKNOWN;
            nfc_build_and_set_details(g_pn532, uid + 1, uid_len);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    nfc_scan_task_handle = NULL;
    vTaskDelete(NULL);
}
#endif

static void update_selected_style_from_theme(void) {
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    lv_color_t theme_bg = lv_color_hex(theme_palettes[theme][0]);
    lv_style_set_bg_color(&style_selected_item, theme_bg);
    lv_style_set_bg_grad_dir(&style_selected_item, LV_GRAD_DIR_NONE);
    lv_style_set_bg_grad_color(&style_selected_item, theme_bg);
}

static void highlight_selected(void) {
    if (!menu_container) return;
    for (int i = 0; i < num_items; ++i) {
        lv_obj_t *child = lv_obj_get_child(menu_container, i);
        if (!child) continue;
        lv_obj_t *label = lv_obj_get_child(child, 0);
        if (i == selected_index) {
            update_selected_style_from_theme();
            lv_obj_add_style(child, &style_selected_item, 0);
            if (label) {
                uint8_t theme = settings_get_menu_theme(&G_Settings);
                if (theme == 3) lv_obj_set_style_text_color(label, lv_color_hex(0x000000), 0);
                else lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
            }
            lv_obj_scroll_to_view(child, LV_ANIM_OFF);
        } else {
            lv_obj_remove_style(child, &style_selected_item, 0);
            if (label) lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
        }
    }
}

static void execute_selected(void) { /* no bottom status text in this view */ }

static void init_styles(void) {
    if (styles_initialized) return;
    lv_style_init(&style_menu_item);
    lv_style_set_bg_color(&style_menu_item, lv_color_hex(0x1E1E1E));
    lv_style_set_bg_opa(&style_menu_item, LV_OPA_COVER);
    lv_style_set_border_width(&style_menu_item, 0);
    lv_style_set_radius(&style_menu_item, 0);

    lv_style_init(&style_menu_item_alt);
    lv_style_set_bg_color(&style_menu_item_alt, lv_color_hex(0x232323));
    lv_style_set_bg_opa(&style_menu_item_alt, LV_OPA_COVER);
    lv_style_set_border_width(&style_menu_item_alt, 0);
    lv_style_set_radius(&style_menu_item_alt, 0);

    lv_style_init(&style_selected_item);
    lv_style_set_bg_opa(&style_selected_item, LV_OPA_COVER);
    lv_style_set_radius(&style_selected_item, 0);

    lv_style_init(&style_menu_label);
    lv_style_set_text_color(&style_menu_label, lv_color_hex(0xFFFFFF));

    styles_initialized = true;
}

static const lv_font_t* get_menu_font(void) { return is_small_screen_global ? &lv_font_montserrat_12 : &lv_font_montserrat_14; }

static void vertically_center_label(lv_obj_t *label, lv_obj_t *btn) {
    if (!label || !btn) return;
    lv_obj_set_style_pad_top(btn, 0, 0);
    float btn_y_center_pad = (button_height_global - lv_font_get_line_height(get_menu_font())) / 2;
    if (btn_y_center_pad < 0) btn_y_center_pad = 0;
    lv_obj_set_style_pad_top(label, btn_y_center_pad, 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
}

// forward declare back_event_cb so it can be used before its definition
static void back_event_cb(lv_event_t *e);
// forward declare option dispatcher used by multiple input paths
static void nfc_option_event_cb(lv_event_t *e);

static void nfc_view_input_cb(InputEvent *event) {
    if (!root) return;
    // Handle NFC scan popup input first
    if (nfc_scan_popup && lv_obj_is_valid(nfc_scan_popup)) {
        if (event->type == INPUT_TYPE_TOUCH) {
            lv_indev_data_t *d = &event->data.touch_data;
            if (d->state == LV_INDEV_STATE_PR) return; // handle on release for consistency with this view
            // Cancel button
            if (nfc_scan_cancel_btn && lv_obj_is_valid(nfc_scan_cancel_btn)) {
                lv_area_t a; lv_obj_get_coords(nfc_scan_cancel_btn, &a);
                if (d->point.x >= a.x1 && d->point.x <= a.x2 && d->point.y >= a.y1 && d->point.y <= a.y2) {
                    nfc_scan_cancel_cb(NULL);
                    return;
                }
            }
            // Save button (if visible)
            if (nfc_save_visible && nfc_scan_save_btn && lv_obj_is_valid(nfc_scan_save_btn)) {
                lv_area_t c; lv_obj_get_coords(nfc_scan_save_btn, &c);
                if (d->point.x >= c.x1 && d->point.x <= c.x2 && d->point.y >= c.y1 && d->point.y <= c.y2) {
                    nfc_scan_save_cb(NULL);
                    return;
                }
            }
            // More button (if visible)
            if (nfc_more_visible && nfc_scan_more_btn && lv_obj_is_valid(nfc_scan_more_btn)) {
                lv_area_t b; lv_obj_get_coords(nfc_scan_more_btn, &b);
                if (d->point.x >= b.x1 && d->point.x <= b.x2 && d->point.y >= b.y1 && d->point.y <= b.y2) {
                    nfc_scan_more_cb(NULL);
                    return;
                }
            }
            update_nfc_popup_selection();
        } else if (event->type == INPUT_TYPE_JOYSTICK) {
            int ji = event->data.joystick_index;
            int total = 1 + (nfc_more_visible ? 1 : 0) + (nfc_save_visible ? 1 : 0);
            if (ji == 2 || ji == 4) { // up/down cycles
                if (total > 1) nfc_popup_selected = (nfc_popup_selected + 1) % total;
                update_nfc_popup_selection();
            } else if (ji == 1) { // select/press
                if (nfc_popup_selected == 0) {
                    nfc_scan_cancel_cb(NULL);
                } else if (nfc_more_visible && nfc_popup_selected == 1) {
                    nfc_scan_more_cb(NULL);
                } else if (nfc_save_visible && ((nfc_more_visible && nfc_popup_selected == 2) || (!nfc_more_visible && nfc_popup_selected == 1))) {
                    nfc_scan_save_cb(NULL);
                }
                return;
            } else if (ji == 0) { // back
                nfc_scan_cancel_cb(NULL);
                return;
            }
        } else if (event->type == INPUT_TYPE_ENCODER) {
            if (event->data.encoder.button) {
                if (nfc_popup_selected == 0) nfc_scan_cancel_cb(NULL);
                else if (nfc_more_visible && nfc_popup_selected == 1) nfc_scan_more_cb(NULL);
                else if (nfc_save_visible && ((nfc_more_visible && nfc_popup_selected == 2) || (!nfc_more_visible && nfc_popup_selected == 1))) nfc_scan_save_cb(NULL);
                return;
            }
            // Rotation toggles selection when More is available
            int total = 1 + (nfc_more_visible ? 1 : 0) + (nfc_save_visible ? 1 : 0);
            if (event->data.encoder.direction != 0 && total > 1) {
                if (event->data.encoder.direction > 0) nfc_popup_selected = (nfc_popup_selected + 1) % total;
                else nfc_popup_selected = (nfc_popup_selected + total - 1) % total;
            }
            update_nfc_popup_selection();
        } else if (event->type == INPUT_TYPE_KEYBOARD) {
            int kv = event->data.key_value;
            if (kv == 9) { // Tab
                int total = 1 + (nfc_more_visible ? 1 : 0) + (nfc_save_visible ? 1 : 0);
                if (total > 1) nfc_popup_selected = (nfc_popup_selected + 1) % total; else nfc_popup_selected = 0;
                update_nfc_popup_selection();
            } else if (kv == 13 || kv == 10) { // Enter
                if (nfc_popup_selected == 0) nfc_scan_cancel_cb(NULL);
                else if (nfc_more_visible && nfc_popup_selected == 1) nfc_scan_more_cb(NULL);
                else if (nfc_save_visible && ((nfc_more_visible && nfc_popup_selected == 2) || (!nfc_more_visible && nfc_popup_selected == 1))) nfc_scan_save_cb(NULL);
                return;
            } else if (kv == 27 || kv == 'c' || kv == 'C') { // Esc or 'c'
                nfc_scan_cancel_cb(NULL);
                return;
            }
            update_nfc_popup_selection();
        }
        return; // consume input while popup is open
    }
    if (event->type == INPUT_TYPE_TOUCH) {
        lv_indev_data_t *d = &event->data.touch_data;
        if (d->state == LV_INDEV_STATE_PR) return; // handle only on release
        int x = d->point.x;
        int y = d->point.y;
        // check buttons
        for (int i = 0; i < num_items; ++i) {
            lv_obj_t *btn = lv_obj_get_child(menu_container, i);
            if (!btn) continue;
            lv_area_t a;
            lv_obj_get_coords(btn, &a);
            if (x >= a.x1 && x <= a.x2 && y >= a.y1 && y <= a.y2) {
                selected_index = i;
                highlight_selected();
                execute_selected();
                return;
            }
        }
        // touch outside -> back
        display_manager_switch_view(&main_menu_view);
    } else if (event->type == INPUT_TYPE_JOYSTICK) {
        int btn = event->data.joystick_index;
        if (btn == 2) { // up
            selected_index = (selected_index - 1 + num_items) % num_items;
            highlight_selected();
        } else if (btn == 4) { // down
            selected_index = (selected_index + 1) % num_items;
            highlight_selected();
        } else if (btn == 1) { // select
            lv_obj_t *selected_obj = lv_obj_get_child(menu_container, selected_index);
            if (selected_obj) {
                const char *opt = (const char *)lv_obj_get_user_data(selected_obj);
                if (opt) {
                    lv_event_t e; memset(&e, 0, sizeof(e)); e.user_data = (void *)opt;
                    nfc_option_event_cb(&e);
                } else {
                    execute_selected();
                }
            } else {
                execute_selected();
            }
        } else if (btn == 0) { // back
            display_manager_switch_view(&main_menu_view);
        }
    } else if (event->type == INPUT_TYPE_ENCODER) {
        if (event->data.encoder.button) {
            lv_obj_t *sel = lv_obj_get_child(menu_container, selected_index);
            if (sel) {
                const char *opt = (const char *)lv_obj_get_user_data(sel);
                if (opt && strcmp(opt, "__BACK_OPTION__") == 0) back_event_cb(NULL);
                else if (opt) { lv_event_t e; memset(&e, 0, sizeof(e)); e.user_data = (void *)opt; nfc_option_event_cb(&e); }
                else execute_selected();
            } else execute_selected();
        } else {
            if (event->data.encoder.direction > 0)
                selected_index = (selected_index + 1) % num_items;
            else
                selected_index = (selected_index - 1 + num_items) % num_items;
            highlight_selected();
        }
    } else if (event->type == INPUT_TYPE_KEYBOARD) {
        int kv = event->data.key_value;
        if (kv == 13) { // Enter
            lv_obj_t *selected_obj = lv_obj_get_child(menu_container, selected_index);
            if (selected_obj) {
                const char *opt = (const char *)lv_obj_get_user_data(selected_obj);
                if (opt) { lv_event_t e; memset(&e, 0, sizeof(e)); e.user_data = (void *)opt; nfc_option_event_cb(&e); }
                else execute_selected();
            } else execute_selected();
        } else if (kv == 44 || kv == ',') { // left/up
            selected_index = (selected_index - 1 + num_items) % num_items;
            highlight_selected();
        } else if (kv == 47 || kv == '/') { // right/down
            selected_index = (selected_index + 1) % num_items;
            highlight_selected();
        } else if (kv == 29 || kv == '`') { // Esc
            display_manager_switch_view(&main_menu_view);
        }
#ifdef CONFIG_USE_ENCODER
    } else if (event->type == INPUT_TYPE_EXIT_BUTTON) {
        display_manager_switch_view(&main_menu_view);
#endif
    }
}

static void nfc_option_event_cb(lv_event_t *e) {
    // user_data is const char* option
    const char *opt = (const char *)lv_event_get_user_data(e);
    if (!opt) return;
    if (strcmp(opt, "__BACK_OPTION__") == 0) {
        back_event_cb(NULL);
        return;
    }

    if (strcmp(opt, "Scan") == 0) {
        create_nfc_scan_popup();
        return;
    }

    // no bottom status label; actions can be wired here if needed
}

// touchscreen scroll callbacks
static void scroll_nfc_up(lv_event_t *e) {
    if (!menu_container) return;
    lv_coord_t scroll_amt = lv_obj_get_height(menu_container) / 2;
    lv_obj_scroll_by_bounded(menu_container, 0, scroll_amt, LV_ANIM_OFF);
}
static void scroll_nfc_down(lv_event_t *e) {
    if (!menu_container) return;
    lv_coord_t scroll_amt = lv_obj_get_height(menu_container) / 2;
    lv_obj_scroll_by_bounded(menu_container, 0, -scroll_amt, LV_ANIM_OFF);
}
static void back_event_cb(lv_event_t *e) {
    display_manager_switch_view(&main_menu_view);
}

static lv_style_t* get_zebra_style(int index) {
    if (settings_get_zebra_menus_enabled(&G_Settings)) return (index % 2 == 0) ? &style_menu_item : &style_menu_item_alt;
    return &style_menu_item;
}

static void cleanup_nfc_scan_popup(void *obj) {
    if (nfc_scan_popup) {
        lv_obj_del(nfc_scan_popup);
        nfc_scan_popup = NULL;
        nfc_scan_cancel_btn = NULL;
        nfc_scan_more_btn = NULL;
        nfc_scan_save_btn = NULL;
        nfc_title_label = NULL;
        nfc_uid_label = NULL;
        nfc_type_label = NULL;
        nfc_details_label = NULL;
    }
#ifdef CONFIG_HAS_NFC
    if (nfc_scan_task_handle) {
        TaskHandle_t h = nfc_scan_task_handle;
        nfc_scan_task_handle = NULL;
        vTaskDelete(h);
    }
    if (nfc_details_text) { free(nfc_details_text); nfc_details_text = NULL; }
    nfc_details_ready = false;
    nfc_details_visible = false;
#endif
}

static void nfc_scan_cancel_cb(lv_event_t *e) {
    cleanup_nfc_scan_popup(NULL);
    display_manager_switch_view(&nfc_view);
}

static void nfc_scan_more_cb(lv_event_t *e) {
    // Toggle details view
    if (!nfc_details_visible) nfc_show_details_view(true);
    else nfc_show_details_view(false);
}

static void nfc_scan_save_cb(lv_event_t *e) {
    write_flipper_nfc_file();
}

static void write_flipper_nfc_file(void) {
    const char *dir = "/mnt/ghostesp/nfc";
    sd_card_create_directory(dir);
#ifdef CONFIG_HAS_NFC
    if (g_uid_len == 0 || g_pn532 == NULL) {
        ESP_LOGW(TAG, "No NFC UID/driver to save");
        return;
    }

    // Build filename: <Model>_<UID>.nfc
    char uid_part[40] = {0};
    int up = 0;
    for (uint8_t i = 0; i < g_uid_len && up < (int)sizeof(uid_part) - 3; ++i) {
        up += snprintf(uid_part + up, sizeof(uid_part) - up, "%02X", g_uid[i]);
        if (i + 1 < g_uid_len) up += snprintf(uid_part + up, sizeof(uid_part) - up, "-");
    }
    const char *model_str = ntag_model_str(g_model);
    char path[192];
    snprintf(path, sizeof(path), "%s/%s_%s.nfc", dir, model_str, uid_part);

    // Determine total pages by model
    int pages_total = 0;
    switch (g_model) {
        case NTAG2XX_NTAG213: pages_total = 45; break;
        case NTAG2XX_NTAG215: pages_total = 135; break;
        case NTAG2XX_NTAG216: pages_total = 231; break;
        default: pages_total = 135; break;
    }

    // Header
    char buf[512];
    int pos = 0;
    pos += snprintf(buf + pos, sizeof(buf) - pos, "Filetype: Flipper NFC device\n");
    pos += snprintf(buf + pos, sizeof(buf) - pos, "Version: 4\n");
    pos += snprintf(buf + pos, sizeof(buf) - pos, "Device type: NTAG/Ultralight\n");
    pos += snprintf(buf + pos, sizeof(buf) - pos, "UID:");
    for (uint8_t i = 0; i < g_uid_len && pos < (int)sizeof(buf) - 4; ++i) {
        pos += snprintf(buf + pos, sizeof(buf) - pos, " %02X", g_uid[i]);
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos, "\n");
    pos += snprintf(buf + pos, sizeof(buf) - pos, "ATQA: %02X %02X\n", (g_atqa >> 8) & 0xFF, g_atqa & 0xFF);
    pos += snprintf(buf + pos, sizeof(buf) - pos, "SAK: %02X\n", g_sak);
    pos += snprintf(buf + pos, sizeof(buf) - pos, "Data format version: 2\n");
    pos += snprintf(buf + pos, sizeof(buf) - pos, "NTAG/Ultralight type: %s\n", model_str);

    if (sd_card_write_file(path, buf, (size_t)pos) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write header: %s", path);
        return;
    }

    // Signature (32 bytes)
    uint8_t sig[32];
    if (ntag2xx_read_signature(g_pn532, sig) != ESP_OK) memset(sig, 0, sizeof(sig));
    pos = 0;
    pos += snprintf(buf + pos, sizeof(buf) - pos, "Signature:");
    for (int i = 0; i < 32 && pos < (int)sizeof(buf) - 4; ++i) pos += snprintf(buf + pos, sizeof(buf) - pos, " %02X", sig[i]);
    pos += snprintf(buf + pos, sizeof(buf) - pos, "\n");
    sd_card_append_file(path, buf, (size_t)pos);

    // Mifare version (GET_VERSION -> 8 bytes)
    uint8_t ver[8]; if (ntag2xx_get_version(g_pn532, ver) != ESP_OK) memset(ver, 0, sizeof(ver));
    pos = 0;
    pos += snprintf(buf + pos, sizeof(buf) - pos, "Mifare version:");
    for (int i = 0; i < 8 && pos < (int)sizeof(buf) - 4; ++i) pos += snprintf(buf + pos, sizeof(buf) - pos, " %02X", ver[i]);
    pos += snprintf(buf + pos, sizeof(buf) - pos, "\n");
    sd_card_append_file(path, buf, (size_t)pos);

    // Counters and tearing flags
    for (int ci = 0; ci < 3; ++ci) {
        uint32_t cv = 0; uint8_t tr = 0; esp_err_t erc = ntag2xx_read_counter(g_pn532, (uint8_t)ci, &cv); esp_err_t ert = ntag2xx_read_tearing(g_pn532, (uint8_t)ci, &tr);
        pos = 0; pos += snprintf(buf + pos, sizeof(buf) - pos, "Counter %d: %u\n", ci, (erc == ESP_OK) ? (unsigned)cv : 0);
        pos += snprintf(buf + pos, sizeof(buf) - pos, "Tearing %d: %02X\n", ci, (ert == ESP_OK) ? tr : 0);
        sd_card_append_file(path, buf, (size_t)pos);
    }

    // Read all pages and build page dump
    size_t cap = (size_t)pages_total * 48 + 64;
    char *pages = (char*)malloc(cap);
    if (!pages) {
        ESP_LOGE(TAG, "OOM building page dump");
        return;
    }
    int ppos = 0; int pages_read = 0;
    for (int pg = 0; pg < pages_total; pg += 4) {
        uint8_t block[16] = {0};
        if (ntag2xx_read_page(g_pn532, (uint8_t)pg, block, 16) == ESP_OK) {
            int chunk = (pages_total - pg >= 4) ? 4 : (pages_total - pg);
            pages_read += chunk;
        }
        // format up to 4 pages from this block
        for (int off = 0; off < 4 && pg + off < pages_total; ++off) {
            uint8_t *data = &block[off * 4];
            ppos += snprintf(pages + ppos, cap - ppos, "Page %d: %02X %02X %02X %02X\n",
                             pg + off, data[0], data[1], data[2], data[3]);
            if (ppos >= (int)cap - 64) break;
        }
        if (ppos >= (int)cap - 64) break;
    }

    // Pages meta then pages dump
    pos = snprintf(buf, sizeof(buf), "Pages total: %d\nPages read: %d\n", pages_total, pages_read);
    sd_card_append_file(path, buf, (size_t)pos);
    sd_card_append_file(path, pages, (size_t)ppos);
    free(pages);

    // Footer
    const char *footer = "Failed authentication attempts: 0\n";
    sd_card_append_file(path, footer, strlen(footer));

    ESP_LOGI(TAG, "NFC file saved: %s", path);
#else
    ESP_LOGW(TAG, "NFC not enabled; nothing to save");
#endif
}

static void create_nfc_scan_popup(void) {
    if (nfc_scan_popup && lv_obj_is_valid(nfc_scan_popup)) {
        cleanup_nfc_scan_popup(NULL);
    }
    if (!root || !lv_obj_is_valid(root)) return;
    nfc_scan_popup = lv_obj_create(root);
    // scale to screen, leave margin for status bar and edges
    int popup_w = LV_HOR_RES - 30;
    int popup_h = (LV_VER_RES <= 240) ? 140 : 160;
    lv_obj_set_size(nfc_scan_popup, popup_w, popup_h);
    lv_obj_align(nfc_scan_popup, LV_ALIGN_TOP_MID, 0, 24); // below status bar
    lv_obj_set_style_bg_color(nfc_scan_popup, lv_color_hex(0x2E2E2E), 0);
    lv_obj_set_style_border_color(nfc_scan_popup, lv_color_hex(0x555555), 0);
    lv_obj_set_style_border_width(nfc_scan_popup, 2, 0);
    lv_obj_set_style_radius(nfc_scan_popup, 10, 0);
    lv_obj_clear_flag(nfc_scan_popup, LV_OBJ_FLAG_SCROLLABLE);
    // Tighten internal padding to maximize usable space
    lv_obj_set_style_pad_top(nfc_scan_popup, 2, 0);
    lv_obj_set_style_pad_bottom(nfc_scan_popup, 4, 0);
    lv_obj_set_style_pad_left(nfc_scan_popup, 6, 0);
    lv_obj_set_style_pad_right(nfc_scan_popup, 6, 0);

    // Title
    nfc_title_label = lv_label_create(nfc_scan_popup);
    lv_label_set_text(nfc_title_label, "Scanning NFC...");
    const lv_font_t *title_font = (LV_VER_RES <= 240) ? &lv_font_montserrat_14 : &lv_font_montserrat_16;
    lv_obj_set_style_text_font(nfc_title_label, title_font, 0);
    lv_obj_set_style_text_color(nfc_title_label, lv_color_hex(0xFFFFFF), 0);
    // Default non-details: title near top (summary)
    lv_obj_align(nfc_title_label, LV_ALIGN_TOP_MID, 0, 22);

    // Placeholder fields (UID / Type)
    nfc_uid_label = lv_label_create(nfc_scan_popup);
    lv_label_set_text(nfc_uid_label, "UID: -- -- -- -- -- -- -- --");
    lv_obj_set_style_text_color(nfc_uid_label, lv_color_hex(0xCCCCCC), 0);
    const lv_font_t *body_font = (LV_VER_RES <= 240) ? &lv_font_montserrat_12 : &lv_font_montserrat_14;
    lv_obj_set_style_text_font(nfc_uid_label, body_font, 0);
    // Default non-details: summary positions (UID a bit closer to title)
    lv_obj_align(nfc_uid_label, LV_ALIGN_TOP_MID, 0, 40);

    nfc_type_label = lv_label_create(nfc_scan_popup);
    lv_label_set_text(nfc_type_label, "Type: --");
    lv_obj_set_style_text_color(nfc_type_label, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_text_font(nfc_type_label, body_font, 0);
    lv_obj_align(nfc_type_label, LV_ALIGN_TOP_MID, 0, 60);

    // Cancel button
    nfc_scan_cancel_btn = lv_btn_create(nfc_scan_popup);
    int btn_w = 90, btn_h = 34;
    if (LV_VER_RES <= 240) { btn_w = 80; btn_h = 30; }
    lv_obj_set_size(nfc_scan_cancel_btn, btn_w, btn_h);
    lv_obj_set_style_bg_color(nfc_scan_cancel_btn, lv_color_hex(0x444444), 0);
    lv_obj_set_style_border_color(nfc_scan_cancel_btn, lv_color_hex(0x666666), 0);
    lv_obj_set_style_border_width(nfc_scan_cancel_btn, 1, 0);
    lv_obj_t *cancel_label = lv_label_create(nfc_scan_cancel_btn);
    lv_label_set_text(cancel_label, "Cancel");
    lv_obj_set_style_text_font(cancel_label, body_font, 0);
    lv_obj_center(cancel_label);
    lv_obj_add_event_cb(nfc_scan_cancel_btn, nfc_scan_cancel_cb, LV_EVENT_CLICKED, NULL);

    // More button (hidden until a tag is scanned)
    nfc_scan_more_btn = lv_btn_create(nfc_scan_popup);
    lv_obj_set_size(nfc_scan_more_btn, btn_w, btn_h);
    lv_obj_set_style_bg_color(nfc_scan_more_btn, lv_color_hex(0x444444), 0);
    lv_obj_set_style_border_color(nfc_scan_more_btn, lv_color_hex(0x666666), 0);
    lv_obj_set_style_border_width(nfc_scan_more_btn, 1, 0);
    lv_obj_add_flag(nfc_scan_more_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(nfc_scan_more_btn, nfc_scan_more_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *more_label = lv_label_create(nfc_scan_more_btn);
    lv_label_set_text(more_label, "More");
    lv_obj_set_style_text_font(more_label, body_font, 0);
    lv_obj_center(more_label);

    // Save button (hidden until a tag is scanned)
    nfc_scan_save_btn = lv_btn_create(nfc_scan_popup);
    lv_obj_set_size(nfc_scan_save_btn, btn_w, btn_h);
    lv_obj_set_style_bg_color(nfc_scan_save_btn, lv_color_hex(0x444444), 0);
    lv_obj_set_style_border_color(nfc_scan_save_btn, lv_color_hex(0x666666), 0);
    lv_obj_set_style_border_width(nfc_scan_save_btn, 1, 0);
    lv_obj_add_flag(nfc_scan_save_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(nfc_scan_save_btn, nfc_scan_save_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *save_label = lv_label_create(nfc_scan_save_btn);
    lv_label_set_text(save_label, "Save");
    lv_obj_set_style_text_font(save_label, body_font, 0);
    lv_obj_center(save_label);

    // Initial state: only cancel visible, centered
    nfc_more_visible = false;
    nfc_save_visible = false;
    nfc_popup_selected = 0;
    nfc_details_visible = false;
    update_nfc_buttons_layout();
    update_nfc_popup_selection();
#ifdef CONFIG_HAS_NFC
    if (nfc_scan_task_handle == NULL) {
        xTaskCreate(nfc_scan_task, "nfc_scan", 4096, NULL, 5, &nfc_scan_task_handle);
    }
#endif
}

static void update_nfc_popup_selection(void) {
    if (!nfc_scan_cancel_btn) return;
    // Cancel
    if (nfc_popup_selected == 0) {
        lv_obj_set_style_bg_color(nfc_scan_cancel_btn, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(nfc_scan_cancel_btn, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_t *lbl = lv_obj_get_child(nfc_scan_cancel_btn, 0);
        if (lbl) lv_obj_set_style_text_color(lbl, lv_color_hex(0x000000), 0);
    } else {
        lv_obj_set_style_bg_color(nfc_scan_cancel_btn, lv_color_hex(0x444444), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(nfc_scan_cancel_btn, lv_color_hex(0x666666), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_t *lbl = lv_obj_get_child(nfc_scan_cancel_btn, 0);
        if (lbl) lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
    }
    // More
    if (nfc_scan_more_btn && lv_obj_is_valid(nfc_scan_more_btn) && nfc_more_visible) {
        if (nfc_popup_selected == 1) {
            lv_obj_set_style_bg_color(nfc_scan_more_btn, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_border_color(nfc_scan_more_btn, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_t *lbl2 = lv_obj_get_child(nfc_scan_more_btn, 0);
            if (lbl2) lv_obj_set_style_text_color(lbl2, lv_color_hex(0x000000), 0);
        } else {
            lv_obj_set_style_bg_color(nfc_scan_more_btn, lv_color_hex(0x444444), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_border_color(nfc_scan_more_btn, lv_color_hex(0x666666), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_t *lbl2 = lv_obj_get_child(nfc_scan_more_btn, 0);
            if (lbl2) lv_obj_set_style_text_color(lbl2, lv_color_hex(0xFFFFFF), 0);
        }
    }
    // Save
    if (nfc_scan_save_btn && lv_obj_is_valid(nfc_scan_save_btn) && nfc_save_visible) {
        int save_index = nfc_more_visible ? 2 : 1;
        if (nfc_popup_selected == save_index) {
            lv_obj_set_style_bg_color(nfc_scan_save_btn, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_border_color(nfc_scan_save_btn, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_t *lbl3 = lv_obj_get_child(nfc_scan_save_btn, 0);
            if (lbl3) lv_obj_set_style_text_color(lbl3, lv_color_hex(0x000000), 0);
        } else {
            lv_obj_set_style_bg_color(nfc_scan_save_btn, lv_color_hex(0x444444), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_border_color(nfc_scan_save_btn, lv_color_hex(0x666666), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_t *lbl3 = lv_obj_get_child(nfc_scan_save_btn, 0);
            if (lbl3) lv_obj_set_style_text_color(lbl3, lv_color_hex(0xFFFFFF), 0);
        }
    }
}

static void update_nfc_buttons_layout(void) {
    if (!nfc_scan_cancel_btn) return;
    // y offset: move buttons up a bit in summary view; details view remains unchanged at -8
    int yoff = nfc_details_visible ? -8 : -8;
    if (nfc_save_visible && nfc_scan_save_btn && lv_obj_is_valid(nfc_scan_save_btn) &&
        nfc_more_visible && nfc_scan_more_btn && lv_obj_is_valid(nfc_scan_more_btn)) {
        // 3 buttons: Cancel (left), More (mid), Save (right)
        lv_obj_align(nfc_scan_cancel_btn, LV_ALIGN_BOTTOM_LEFT, 10, yoff);
        lv_obj_align(nfc_scan_more_btn, LV_ALIGN_BOTTOM_MID, 0, yoff);
        lv_obj_align(nfc_scan_save_btn, LV_ALIGN_BOTTOM_RIGHT, -10, yoff);
    } else if (nfc_more_visible && nfc_scan_more_btn && lv_obj_is_valid(nfc_scan_more_btn)) {
        // Cancel (left), More (right)
        lv_obj_align(nfc_scan_cancel_btn, LV_ALIGN_BOTTOM_LEFT, 10, yoff);
        lv_obj_align(nfc_scan_more_btn, LV_ALIGN_BOTTOM_RIGHT, -10, yoff);
    } else if (nfc_save_visible && nfc_scan_save_btn && lv_obj_is_valid(nfc_scan_save_btn)) {
        // Cancel (left), Save (right)
        lv_obj_align(nfc_scan_cancel_btn, LV_ALIGN_BOTTOM_LEFT, 10, yoff);
        lv_obj_align(nfc_scan_save_btn, LV_ALIGN_BOTTOM_RIGHT, -10, yoff);
    } else {
        // Only Cancel
        lv_obj_align(nfc_scan_cancel_btn, LV_ALIGN_BOTTOM_MID, 0, yoff);
    }
}

static void nfc_show_details_view(bool show) {
    if (!nfc_scan_popup || !lv_obj_is_valid(nfc_scan_popup)) return;
    const lv_font_t *body_font = (LV_VER_RES <= 240) ? &lv_font_montserrat_12 : &lv_font_montserrat_14;
    if (show) {
        // Hide summary fields
        if (nfc_uid_label) lv_obj_add_flag(nfc_uid_label, LV_OBJ_FLAG_HIDDEN);
        if (nfc_type_label) lv_obj_add_flag(nfc_type_label, LV_OBJ_FLAG_HIDDEN);
        // Title and button label
        if (nfc_title_label && lv_obj_is_valid(nfc_title_label)) {
            lv_label_set_text(nfc_title_label, "NFC Details");
            // Details title slightly down from top for spacing
            lv_obj_align(nfc_title_label, LV_ALIGN_TOP_MID, 0, 4);
        }
        if (nfc_scan_more_btn && lv_obj_is_valid(nfc_scan_more_btn)) {
            lv_obj_t *lbl = lv_obj_get_child(nfc_scan_more_btn, 0);
            if (lbl) lv_label_set_text(lbl, "Less");
        }
        // Create details label if needed
        if (!nfc_details_label || !lv_obj_is_valid(nfc_details_label)) {
            nfc_details_label = lv_label_create(nfc_scan_popup);
            lv_obj_set_width(nfc_details_label, LV_HOR_RES - 50);
            lv_label_set_long_mode(nfc_details_label, LV_LABEL_LONG_WRAP);
            lv_obj_set_style_text_font(nfc_details_label, body_font, 0);
            lv_obj_set_style_text_color(nfc_details_label, lv_color_hex(0xDDDDDD), 0);
        }
        // Align details label every time details view is shown (more spacing from title)
        if (nfc_details_label && lv_obj_is_valid(nfc_details_label)) {
            lv_obj_align(nfc_details_label, LV_ALIGN_TOP_MID, 0, 14);
        }
        // Set details text
        #ifdef CONFIG_HAS_NFC
        if (nfc_details_ready && nfc_details_text) {
            lv_label_set_text(nfc_details_label, nfc_details_text);
        } else {
            lv_label_set_text(nfc_details_label, "Reading tag data...");
        }
        #else
        lv_label_set_text(nfc_details_label, "NFC not available");
        #endif
        lv_obj_clear_flag(nfc_details_label, LV_OBJ_FLAG_HIDDEN);
        nfc_details_visible = true;
        nfc_popup_selected = 1; // focus Less button
        update_nfc_popup_selection();
    } else {
        // Hide details, show summary
        if (nfc_details_label && lv_obj_is_valid(nfc_details_label)) lv_obj_add_flag(nfc_details_label, LV_OBJ_FLAG_HIDDEN);
        if (nfc_uid_label) lv_obj_clear_flag(nfc_uid_label, LV_OBJ_FLAG_HIDDEN);
        if (nfc_type_label) lv_obj_clear_flag(nfc_type_label, LV_OBJ_FLAG_HIDDEN);
        if (nfc_title_label && lv_obj_is_valid(nfc_title_label)) {
            lv_label_set_text(nfc_title_label, "NFC Tag");
            lv_obj_align(nfc_title_label, LV_ALIGN_TOP_MID, 0, 22);
        }
        if (nfc_scan_more_btn && lv_obj_is_valid(nfc_scan_more_btn)) {
            lv_obj_t *lbl = lv_obj_get_child(nfc_scan_more_btn, 0);
            if (lbl) lv_label_set_text(lbl, "More");
        }
        nfc_details_visible = false;
        nfc_popup_selected = 0; // focus Cancel
        update_nfc_popup_selection();
        // Update buttons layout for summary spacing
        update_nfc_buttons_layout();
    }
}

static void nfc_view_create(void) {
    lv_obj_clear_flag(lv_scr_act(), LV_OBJ_FLAG_SCROLLABLE);
    root = lv_obj_create(lv_scr_act());
    nfc_view.root = root;
    lv_obj_set_size(root, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(root, lv_color_hex(0x121212), 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_align(root, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_scrollbar_mode(root, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    init_styles();

    int screen_width = LV_HOR_RES;
    int screen_height = LV_VER_RES;
    const int STATUS_BAR_HEIGHT = 20;
    bool is_small_screen = (screen_width <= 240 || screen_height <= 240);
#ifdef CONFIG_USE_TOUCHSCREEN
    const int BUTTON_AREA_HEIGHT = SCROLL_BTN_SIZE + SCROLL_BTN_PADDING * 2;
#else
    const int BUTTON_AREA_HEIGHT = 0;
#endif
    int container_height = screen_height - STATUS_BAR_HEIGHT - BUTTON_AREA_HEIGHT;

    is_small_screen_global = is_small_screen;
    button_height_global = is_small_screen ? 40 : 55;

    menu_container = lv_list_create(root);
    lv_obj_set_style_radius(menu_container, 0, LV_PART_MAIN);
    lv_obj_set_size(menu_container, screen_width, container_height);
    lv_obj_align(menu_container, LV_ALIGN_TOP_MID, 0, STATUS_BAR_HEIGHT);
    lv_obj_set_style_bg_color(menu_container, lv_color_hex(0x121212), 0);
    lv_obj_set_style_pad_all(menu_container, 0, 0);
    lv_obj_set_style_pad_top(menu_container, 0, 0);
    lv_obj_set_style_pad_bottom(menu_container, 0, 0);
    lv_obj_set_style_border_width(menu_container, 0, 0);
    lv_obj_set_scrollbar_mode(menu_container, LV_SCROLLBAR_MODE_OFF);

    // Add Scan button
    scan_btn = lv_list_add_btn(menu_container, NULL, "Scan");
    lv_obj_set_height(scan_btn, button_height_global);
    lv_obj_add_style(scan_btn, get_zebra_style(0), 0);
    lv_obj_t *slabel = lv_obj_get_child(scan_btn, 0);
    if (slabel) {
        lv_obj_set_style_text_font(slabel, get_menu_font(), 0);
        vertically_center_label(slabel, scan_btn);
        lv_obj_add_style(slabel, &style_menu_label, 0);
    }
    lv_obj_set_user_data(scan_btn, (void *)"Scan");
    lv_obj_add_event_cb(scan_btn, nfc_option_event_cb, LV_EVENT_CLICKED, (void *)"Scan");

    // Add Emulate button
    emulate_btn = lv_list_add_btn(menu_container, NULL, "Emulate NFC Tag");
    lv_obj_set_height(emulate_btn, button_height_global);
    lv_obj_add_style(emulate_btn, get_zebra_style(1), 0);
    lv_obj_t *elabel = lv_obj_get_child(emulate_btn, 0);
    if (elabel) {
        lv_obj_set_style_text_font(elabel, get_menu_font(), 0);
        vertically_center_label(elabel, emulate_btn);
        lv_obj_add_style(elabel, &style_menu_label, 0);
    }
    lv_obj_set_user_data(emulate_btn, (void *)"Emulate NFC Tag");
    lv_obj_add_event_cb(emulate_btn, nfc_option_event_cb, LV_EVENT_CLICKED, (void *)"Emulate NFC Tag");

    num_items = 2;

#ifdef CONFIG_USE_ENCODER
    // Add Back option row for encoder users, mirroring options_screen behavior
    lv_obj_t *back_row = lv_list_add_btn(menu_container, NULL, LV_SYMBOL_LEFT " Back");
    if (back_row) {
        lv_obj_set_height(back_row, button_height_global);
        lv_obj_add_style(back_row, get_zebra_style(2), 0);
        lv_obj_t *blabel = lv_obj_get_child(back_row, 0);
        if (blabel) {
            lv_obj_set_style_text_font(blabel, get_menu_font(), 0);
            vertically_center_label(blabel, back_row);
            lv_obj_add_style(blabel, &style_menu_label, 0);
        }
        lv_obj_set_user_data(back_row, (void *)"__BACK_OPTION__");
        lv_obj_add_event_cb(back_row, nfc_option_event_cb, LV_EVENT_CLICKED, (void *)"__BACK_OPTION__");
        num_items++;
    }
#endif

    // add touchscreen nav buttons + back button (same style as options_screen)
#ifdef CONFIG_USE_TOUCHSCREEN
    scroll_up_btn = lv_btn_create(root);
    lv_obj_set_size(scroll_up_btn, SCROLL_BTN_SIZE, SCROLL_BTN_SIZE);
    lv_obj_align(scroll_up_btn, LV_ALIGN_BOTTOM_LEFT, SCROLL_BTN_PADDING, -SCROLL_BTN_PADDING);
    lv_obj_set_style_bg_color(scroll_up_btn, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_radius(scroll_up_btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_border_width(scroll_up_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(scroll_up_btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(scroll_up_btn, scroll_nfc_up, LV_EVENT_CLICKED, NULL);
    lv_obj_t *up_label = lv_label_create(scroll_up_btn);
    lv_label_set_text(up_label, LV_SYMBOL_UP);
    lv_obj_center(up_label);

    scroll_down_btn = lv_btn_create(root);
    lv_obj_set_size(scroll_down_btn, SCROLL_BTN_SIZE, SCROLL_BTN_SIZE);
    lv_obj_align(scroll_down_btn, LV_ALIGN_BOTTOM_RIGHT, -SCROLL_BTN_PADDING, -SCROLL_BTN_PADDING);
    lv_obj_set_style_bg_color(scroll_down_btn, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_radius(scroll_down_btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_border_width(scroll_down_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(scroll_down_btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(scroll_down_btn, scroll_nfc_down, LV_EVENT_CLICKED, NULL);
    lv_obj_t *down_label = lv_label_create(scroll_down_btn);
    lv_label_set_text(down_label, LV_SYMBOL_DOWN);
    lv_obj_center(down_label);

    back_btn = lv_btn_create(root);
    lv_obj_set_size(back_btn, SCROLL_BTN_SIZE + 20, SCROLL_BTN_SIZE);
    lv_obj_align(back_btn, LV_ALIGN_BOTTOM_MID, 0, -SCROLL_BTN_PADDING);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0x555555), LV_PART_MAIN);
    lv_obj_set_style_radius(back_btn, 5, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(back_btn, 10, LV_PART_MAIN);
    lv_obj_set_style_border_width(back_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(back_btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(back_btn, back_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_label = lv_label_create(back_btn);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT " Back");
    lv_obj_center(back_label);
#endif

    highlight_selected();

    display_manager_add_status_bar("NFC");
}

static void nfc_view_destroy(void) {
    if (root) {
        lv_obj_del(root);
        root = NULL;
    }
    nfc_view.root = NULL;
    menu_container = NULL;
    scan_btn = NULL;
    emulate_btn = NULL;
    scroll_up_btn = NULL;
    scroll_down_btn = NULL;
    back_btn = NULL;
    nfc_scan_popup = NULL;
    nfc_scan_cancel_btn = NULL;
    nfc_title_label = NULL;
    nfc_uid_label = NULL;
    nfc_type_label = NULL;
    nfc_details_label = NULL;
}

static void get_nfc_callback(void **cb) {
    if (cb) *cb = nfc_view_input_cb;
}

View nfc_view = {
    .root = NULL,
    .create = nfc_view_create,
    .destroy = nfc_view_destroy,
    .input_callback = nfc_view_input_cb,
    .name = "NFC",
    .get_hardwareinput_callback = get_nfc_callback
};
