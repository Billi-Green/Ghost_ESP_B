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
#include "managers/nfc/ntag_t2.h"
#include "managers/nfc/mifare_classic.h"
#endif

// UI hook from MIFARE Classic layer to indicate sector/block/key phase
// (implementation declared later after static variables are defined)
void mfc_ui_set_phase(int sector, int first_block, bool key_b, int total_keys);

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

// UI hook from MIFARE Classic layer to indicate sector/block/key phase
// (implementation moved below after static phase variables are declared)
void mfc_ui_set_phase(int sector, int first_block, bool key_b, int total_keys);

static int button_height_global = 0;
static bool is_small_screen_global = false;

// NFC scan popup (modeled after IR learning popup)
static lv_obj_t *nfc_scan_popup = NULL;
static lv_obj_t *nfc_scan_cancel_btn = NULL;
static lv_obj_t *nfc_scan_more_btn = NULL;
static lv_obj_t *nfc_scan_save_btn = NULL;
static lv_obj_t *nfc_scan_attack_btn = NULL;
static lv_obj_t *nfc_title_label = NULL;
static lv_obj_t *nfc_uid_label = NULL;
static lv_obj_t *nfc_type_label = NULL;
static lv_obj_t *nfc_details_label = NULL;
// Progress bar removed; we will update title and text instead
// Track dictionary brute-force phase for richer UI status
static int mfc_phase_sector = -1;
static int mfc_phase_first_block = -1;
static bool mfc_phase_key_b = false;
static int mfc_phase_total = 0;
static int nfc_popup_selected = 0; // 0 = Cancel, 1 = More (when available)
static bool nfc_more_visible = false;
static bool nfc_details_visible = false;
static bool nfc_save_visible = false;
static bool nfc_attack_visible = false;
// When true, the MFC layer is performing a second-pass cache fill (live-read) after bruteforce.
static bool nfc_cache_fill_phase = false;
// When true, UI requests to skip dictionary attempts (basic read only)
static bool nfc_dict_skip_requested = false;
// When true, a tag was removed and we're waiting for re-present
static bool nfc_paused = false;
// When true, NFC details are ready and scan is complete
static bool nfc_details_ready = false;

// Async setter for paused title/state
static void nfc_set_paused_async(void *ptr) {
    bool on = (ptr != NULL);
    nfc_paused = on;
    if (nfc_title_label && lv_obj_is_valid(nfc_title_label)) {
        if (on) {
            lv_label_set_text(nfc_title_label, "Paused - present tag to continue");
        } else {
            // Restore appropriate title depending on phase
            if (nfc_cache_fill_phase) lv_label_set_text(nfc_title_label, "Reading sectors... 0%");
            else if (!nfc_details_visible) lv_label_set_text(nfc_title_label, "Bruteforcing keys... 0%");
            else { lv_label_set_text(nfc_title_label, "NFC Tag"); lv_obj_align(nfc_title_label, LV_ALIGN_TOP_MID, 0, 22); }
        }
    }
}

// Exposed to MFC layer
void mfc_ui_set_paused(bool on) {
    lv_async_call(nfc_set_paused_async, on ? (void*)1 : NULL);
}

// Async setter for cache fill phase title/state
static void nfc_set_cache_mode_async(void *ptr) {
    bool on = (ptr != NULL);
    nfc_cache_fill_phase = on;
    if (nfc_title_label && lv_obj_is_valid(nfc_title_label)) {
        if (on) lv_label_set_text(nfc_title_label, "Reading sectors... 0%");
        else { lv_label_set_text(nfc_title_label, "NFC Tag"); lv_obj_align(nfc_title_label, LV_ALIGN_TOP_MID, 0, 22); }
    }
}

// Exposed to MFC layer to toggle cache-fill phase
void mfc_ui_set_cache_mode(bool on) {
    lv_async_call(nfc_set_cache_mode_async, on ? (void*)1 : NULL);
}

#ifdef CONFIG_HAS_NFC
// Exposed for mifare_classic.c to honor UI skip request (weak extern there)
bool nfc_is_dict_skip_requested(void) { return nfc_dict_skip_requested; }
#endif
static void nfc_scan_cancel_cb(lv_event_t *e);
static void nfc_scan_more_cb(lv_event_t *e);
static void nfc_scan_save_cb(lv_event_t *e);
static void create_nfc_scan_popup(void);
static void cleanup_nfc_scan_popup(void *obj);
static void update_nfc_popup_selection(void);
static void update_nfc_buttons_layout(void);
static void nfc_show_details_view(bool show);
static bool write_flipper_nfc_file(void);
// Worker task helpers
static void nfc_save_task(void *arg);
static void nfc_save_done_async(void *ptr);
// Deferred scan start if previous scan task hasn't exited yet
static void nfc_try_start_scan_timer_cb(lv_timer_t *t);

// Dictionary progress callback -> UI updater
static void nfc_progress_update_async(void *ptr);
// UI hook from MIFARE Classic layer to indicate sector/block/key phase (implementation)
void mfc_ui_set_phase(int sector, int first_block, bool key_b, int total_keys) {
    // just update phase state and schedule a 0%% progress update on LVGL thread
    mfc_phase_sector = sector;
    mfc_phase_first_block = first_block;
    mfc_phase_key_b = key_b;
    mfc_phase_total = total_keys;
    typedef struct { int c; int t; } dict_prog_t;
    dict_prog_t *dp = (dict_prog_t*)malloc(sizeof(dict_prog_t));
    if (dp) { dp->c = 0; dp->t = total_keys; lv_async_call(nfc_progress_update_async, dp); }
}
static void mfc_dict_progress_cb(int current, int total, void *user) {
    (void)user;
    if (total <= 0) return;
    int percent = (current * 100) / total;
    if (percent < 0) { percent = 0; }
    if (percent > 100) { percent = 100; }
    static int last_percent = -1;
    if (percent == last_percent) return;
    last_percent = percent;
    typedef struct { int c; int t; } dict_prog_t;
    dict_prog_t *dp = (dict_prog_t*)malloc(sizeof(dict_prog_t));
    if (!dp) return;
    dp->c = current; dp->t = total;
    lv_async_call(nfc_progress_update_async, dp);
}

static void nfc_progress_update_async(void *ptr) {
    if (!ptr) return;
    typedef struct { int c; int t; } dict_prog_t;
    dict_prog_t *dp = (dict_prog_t*)ptr;
    if (!nfc_scan_popup || !lv_obj_is_valid(nfc_scan_popup)) { free(dp); return; }
    int percent = 0; if (dp->t > 0) percent = (dp->c * 100) / dp->t; if (percent > 100) percent = 100; if (percent < 0) percent = 0;
    // Compose phase suffix e.g., "Sec 3 Blk 12 Key A"
    char phase[40];
    if (mfc_phase_sector >= 0 && mfc_phase_first_block >= 0) {
        snprintf(phase, sizeof(phase), " | Sec %d Blk %d Key %c", mfc_phase_sector, mfc_phase_first_block, mfc_phase_key_b ? 'B' : 'A');
    } else {
        phase[0] = '\0';
    }
    // Update title with progress only (no phase in title)
    if (nfc_title_label && lv_obj_is_valid(nfc_title_label)) {
        char title[80];
        if (nfc_paused) snprintf(title, sizeof(title), "Paused - present tag to continue");
        else if (nfc_cache_fill_phase) snprintf(title, sizeof(title), "Reading sectors... %d%%", percent);
        else if (nfc_dict_skip_requested) snprintf(title, sizeof(title), "Basic read (skipping dict) ...");
        else if (nfc_details_ready) snprintf(title, sizeof(title), "NFC Tag");
        else snprintf(title, sizeof(title), "Bruteforcing keys... %d%%", percent);
        lv_label_set_text(nfc_title_label, title);
        if (nfc_details_ready) lv_obj_align(nfc_title_label, LV_ALIGN_TOP_MID, 0, 22);
    }
    // While bruteforcing, repurpose More button as Skip (label only; callback decides behavior)
    // Only show Skip button if we're actively bruteforcing (not completed)
    if (!nfc_paused && !nfc_cache_fill_phase && !nfc_dict_skip_requested && !nfc_details_ready && 
        nfc_scan_more_btn && lv_obj_is_valid(nfc_scan_more_btn)) {
        lv_obj_clear_flag(nfc_scan_more_btn, LV_OBJ_FLAG_HIDDEN);
        nfc_more_visible = true;
        // Change label to "Skip"
        lv_obj_t *lbl = lv_obj_get_child(nfc_scan_more_btn, 0);
        if (lbl) lv_label_set_text(lbl, "Skip");
        update_nfc_buttons_layout();
        update_nfc_popup_selection();
    }
    // Replace UID/Type lines with sector/block and key when phase is valid
    if (mfc_phase_sector >= 0 && mfc_phase_first_block >= 0) {
        if (nfc_uid_label && lv_obj_is_valid(nfc_uid_label)) {
            char l1[32];
            snprintf(l1, sizeof(l1), "Sec %d Blk %d", mfc_phase_sector, mfc_phase_first_block);
            lv_label_set_text(nfc_uid_label, l1);
        }
        if (nfc_type_label && lv_obj_is_valid(nfc_type_label)) {
            char l2[16];
            snprintf(l2, sizeof(l2), "Key %c", mfc_phase_key_b ? 'B' : 'A');
            lv_label_set_text(nfc_type_label, l2);
        }
    }
    // If details text is visible, show numeric progress and phase too
    if (nfc_details_label && lv_obj_is_valid(nfc_details_label)) {
        char info[96];
        if (dp->t > 0) snprintf(info, sizeof(info), "Dictionary: %d/%d (%d%%)%s", dp->c, dp->t, percent, phase);
        else snprintf(info, sizeof(info), "Dictionary: %d (unknown total)%s", dp->c, phase);
        lv_label_set_text(nfc_details_label, info);
    }
    free(dp);
}

static volatile bool nfc_scan_cancel = false;
static volatile bool nfc_save_in_progress = false;
static volatile bool nfc_attack_in_progress = false;

// Expose cancel status to MIFARE Classic layer (cooperative cancellation)
bool nfc_is_scan_cancelled(void) { return nfc_scan_cancel; }

#ifdef CONFIG_HAS_NFC
static pn532_io_handle_t g_pn532 = NULL;
static pn532_io_t g_pn532_instance;
static TaskHandle_t nfc_scan_task_handle = NULL;
static char *nfc_details_text = NULL;
static uint8_t g_uid[10] = {0};
static uint8_t g_uid_len = 0;
static uint16_t g_atqa = 0;
static uint8_t g_sak = 0;
static NTAG2XX_MODEL g_model = NTAG2XX_UNKNOWN;


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
    // Reset phase state and update summary labels to indicate completion
    mfc_phase_sector = -1;
    mfc_phase_first_block = -1;
    // Revert label back to More after bruteforce completes
    if (nfc_scan_more_btn && lv_obj_is_valid(nfc_scan_more_btn)) {
        lv_obj_t *lbl = lv_obj_get_child(nfc_scan_more_btn, 0);
        if (lbl) lv_label_set_text(lbl, "More");
        lv_obj_clear_state(nfc_scan_more_btn, LV_STATE_DISABLED);
    }
    if (nfc_title_label && lv_obj_is_valid(nfc_title_label)) {
        lv_label_set_text(nfc_title_label, "NFC Tag");
        lv_obj_align(nfc_title_label, LV_ALIGN_TOP_MID, 0, 22);
    }
    if (!nfc_details_visible) {
        if (nfc_type_label && lv_obj_is_valid(nfc_type_label)) {
            lv_label_set_text(nfc_type_label, "Scan complete - press More");
        }
    }
    // If already showing details, update label
    if (nfc_details_visible && nfc_details_label && lv_obj_is_valid(nfc_details_label)) {
        lv_label_set_text(nfc_details_label, nfc_details_text);
    }
    // Reset dict-skip flag for next scans
    nfc_dict_skip_requested = false;

    // Reveal Save button now that details (and cache) are ready
    if (nfc_scan_save_btn && lv_obj_is_valid(nfc_scan_save_btn) && !nfc_save_visible) {
        lv_obj_clear_flag(nfc_scan_save_btn, LV_OBJ_FLAG_HIDDEN);
        nfc_save_visible = true;
        update_nfc_buttons_layout();
        update_nfc_popup_selection();
    }

    // Resume normal I2C activity now that scanning/bruteforce has finished
    display_manager_set_low_i2c_mode(false);

    free(res);
}



static void nfc_build_and_set_details(pn532_io_handle_t io, const uint8_t *uid, uint8_t uid_len) {
    // Prefer MIFARE Classic summary if SAK indicates Classic
    if (mfc_is_classic_sak(g_sak)) {
        mfc_set_progress_callback(mfc_dict_progress_cb, NULL);
        if (nfc_title_label && lv_obj_is_valid(nfc_title_label)) lv_label_set_text(nfc_title_label, "Bruteforcing keys... 0%");
        // Reduce I2C contention during PN532 scanning/bruteforce
        display_manager_set_low_i2c_mode(true);
        char *text = mfc_build_details_summary(io, uid, uid_len, g_atqa, g_sak);
        if (!text) return;
        ndef_details_result_t *res = (ndef_details_result_t*)malloc(sizeof(*res));
        if (!res) { free(text); return; }
        res->text = text; res->text_len = strlen(text);
        lv_async_call(nfc_set_details_async, res);
        mfc_set_progress_callback(NULL, NULL);
        return;
    }

    // Otherwise try NTAG/Ultralight (Type 2)
    uint8_t *mem = NULL; size_t mem_len = 0; NTAG2XX_MODEL model = NTAG2XX_UNKNOWN;
    if (!ntag_t2_read_user_memory(io, &mem, &mem_len, &model)) {
        size_t cap = 256;
        ndef_details_result_t *res = (ndef_details_result_t*)malloc(sizeof(*res));
        if (!res) return;
        res->text = (char*)malloc(cap);
        res->text_len = cap;
        if (!res->text) { free(res); return; }
        char *w = res->text; snprintf(w, cap, "UID:"); size_t used = strlen(w); w += used; cap -= used;
        for (uint8_t i = 0; i < uid_len && cap > 3; ++i) { int n = snprintf(w, cap, " %02X", uid[i]); if (n>0){ w+=n; cap-=n; } }
        snprintf(w, cap, "\nNo NDEF data\n");
        lv_async_call(nfc_set_details_async, res);
        return;
    }
    char *text = ntag_t2_build_details_from_mem(mem, mem_len, uid, uid_len, model);
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
    // Save remains hidden until details are ready (shown in nfc_set_details_async)
    update_nfc_buttons_layout();
    update_nfc_popup_selection();
    free(uid);
}

static void nfc_scan_task(void *arg) {
    const char *TAGT = "NFCScan";
    ESP_LOGI(TAGT, "scan_task: start (cancel=%d)", nfc_scan_cancel);
    if (g_pn532 == NULL) {
        g_pn532 = &g_pn532_instance;
        // Prefer a single I2C controller for all devices sharing the same pins.
        // Match the Fuel Gauge manager's chosen port by target to avoid two controllers
        // driving the same physical SDA/SCL.
#if defined(CONFIG_HAS_FUEL_GAUGE) || defined(CONFIG_USE_BQ27220_FUEL_GAUGE)
    #if defined(CONFIG_IDF_TARGET_ESP32S3)
        // Use I2C_NUM_0 exclusively to share controller with fuel gauge
        i2c_port_t try_ports[2] = { I2C_NUM_0, I2C_NUM_0 };
    #else
        i2c_port_t try_ports[2] = { I2C_NUM_0, I2C_NUM_1 };
    #endif
#else
        i2c_port_t try_ports[2] = { I2C_NUM_0, I2C_NUM_1 };
#endif
        bool ok = false;
        for (int pi = 0; pi < 2 && !ok; ++pi) {
            i2c_port_t port = try_ports[pi];
            ESP_LOGI(TAGT, "attempting PN532 on I2C port %d", (int)port);
            if (pn532_new_driver_i2c(
                    (gpio_num_t)CONFIG_NFC_SDA_PIN,
                    (gpio_num_t)CONFIG_NFC_SCL_PIN,
                    (gpio_num_t)CONFIG_NFC_RST_PIN,
                    (gpio_num_t)CONFIG_NFC_IRQ_PIN,
                    port,
                    g_pn532) != ESP_OK) {
                ESP_LOGE(TAGT, "pn532_new_driver_i2c failed (port=%d)", (int)port);
                // ensure clean slate before next attempt
                pn532_delete_driver(g_pn532);
                continue;
            }
            if (pn532_init(g_pn532) == ESP_OK) {
                pn532_set_passive_activation_retries(g_pn532, 0xFF);
                ESP_LOGI(TAGT, "scan_task: PN532 initialized on port %d", (int)port);
                ok = true;
            } else {
                ESP_LOGE(TAGT, "pn532_init failed (port=%d)", (int)port);
                pn532_release(g_pn532);
                pn532_delete_driver(g_pn532);
            }
        }
        if (!ok) {
            ESP_LOGE(TAGT, "PN532 init failed on all ports, exiting scan task");
            nfc_scan_task_handle = NULL;
            vTaskDelete(NULL);
        }
    }

    while (!nfc_scan_cancel) {
        uint8_t uid[8] = {0};
        uint8_t uid_len = 0;
        uint16_t atqa = 0; uint8_t sak = 0;
        esp_err_t r = pn532_read_passive_target_id_ex(g_pn532, 0x00, uid + 1, &uid_len, &atqa, &sak, 200);
        if (r == ESP_OK && uid_len > 0 && uid_len <= 7) {
            uid[0] = uid_len;
            uint8_t *copy = (uint8_t *)malloc(uid_len + 1);
            if (copy) {
                memcpy(copy, uid, uid_len + 1);
                lv_async_call(nfc_update_labels_async, copy);
            }
            // Build detailed info synchronously before exiting the task
            if (nfc_scan_cancel) break;
            g_uid_len = uid_len; memcpy(g_uid, uid + 1, uid_len); g_atqa = atqa; g_sak = sak; g_model = NTAG2XX_UNKNOWN;
            ESP_LOGI(TAGT, "scan_task: UID found, building details (len=%u)", uid_len);
            nfc_build_and_set_details(g_pn532, uid + 1, uid_len);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    // Release PN532 resources when scan stops (either by cancel or completion)
    if (g_pn532) {
        if (nfc_scan_cancel) {
            ESP_LOGI(TAGT, "scan_task: releasing PN532 (cancel=%d)", nfc_scan_cancel);
            pn532_release(g_pn532);
            pn532_delete_driver(g_pn532);
            g_pn532 = NULL;
        } else {
            ESP_LOGI(TAGT, "scan_task: keeping PN532 for Save (cancel=%d)", nfc_scan_cancel);
        }
    }
    nfc_scan_task_handle = NULL;
    ESP_LOGI(TAGT, "scan_task: exit");
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
    ESP_LOGI(TAG, "cleanup_nfc_scan_popup: begin (task=%p, cancel=%d)", (void*)nfc_scan_task_handle, nfc_scan_cancel);
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
    // Signal the scan task to exit gracefully (avoid calling LVGL from that task)
    nfc_scan_cancel = true;
    // If scan task already ended, release PN532 here to avoid leaking the handle
    if (g_pn532 && nfc_scan_task_handle == NULL) {
        pn532_release(g_pn532);
        pn532_delete_driver(g_pn532);
        g_pn532 = NULL;
    }
    if (nfc_details_text) { free(nfc_details_text); nfc_details_text = NULL; }
    nfc_details_ready = false;
    nfc_details_visible = false;
    mfc_set_progress_callback(NULL, NULL);
#endif
    ESP_LOGI(TAG, "cleanup_nfc_scan_popup: end (task=%p, cancel=%d)", (void*)nfc_scan_task_handle, nfc_scan_cancel);
}

static void nfc_scan_cancel_cb(lv_event_t *e) {
    cleanup_nfc_scan_popup(NULL);
}

static void nfc_scan_more_cb(lv_event_t *e) {
    (void)e;
    // If bruteforcing is active, treat More as Skip (basic read)
    if (!nfc_dict_skip_requested && mfc_phase_sector >= 0) {
        nfc_dict_skip_requested = true;
        if (nfc_title_label && lv_obj_is_valid(nfc_title_label)) {
            lv_label_set_text(nfc_title_label, "Basic read (skipping dict) ...");
        }
        // Update button to reflect action taken
        if (nfc_scan_more_btn && lv_obj_is_valid(nfc_scan_more_btn)) {
            lv_obj_t *lbl = lv_obj_get_child(nfc_scan_more_btn, 0);
            if (lbl) lv_label_set_text(lbl, "Skipping...");
            lv_obj_add_state(nfc_scan_more_btn, LV_STATE_DISABLED);
        }
        return;
    }
    // Otherwise toggle details view
    if (!nfc_details_visible) nfc_show_details_view(true);
    else nfc_show_details_view(false);
}

static void nfc_scan_save_cb(lv_event_t *e) {
    if (nfc_save_in_progress) return;
    nfc_save_in_progress = true;
    if (nfc_title_label && lv_obj_is_valid(nfc_title_label)) {
        lv_label_set_text(nfc_title_label, "Saving...");
    }
    if (nfc_scan_save_btn && lv_obj_is_valid(nfc_scan_save_btn)) {
        lv_obj_add_state(nfc_scan_save_btn, LV_STATE_DISABLED);
    }
#ifdef CONFIG_HAS_NFC
    mfc_set_progress_callback(mfc_dict_progress_cb, NULL);
#endif
    xTaskCreate(nfc_save_task, "nfc_save", 6144, NULL, 5, NULL);
}

static bool write_flipper_nfc_file(void) {
    const char *dir = "/mnt/ghostesp/nfc";
    sd_card_create_directory(dir);
#ifdef CONFIG_HAS_NFC
    if (g_uid_len == 0 || g_pn532 == NULL) {
        ESP_LOGW(TAG, "No NFC UID/driver to save");
        return false;
    }

    // Build filename: <Model>_<UID>.nfc
    char uid_part[40] = {0};
    int up = 0;
    for (uint8_t i = 0; i < g_uid_len && up < (int)sizeof(uid_part) - 3; ++i) {
        up += snprintf(uid_part + up, sizeof(uid_part) - up, "%02X", g_uid[i]);
        if (i + 1 < g_uid_len) up += snprintf(uid_part + up, sizeof(uid_part) - up, "-");
    }
    char path[192];

    if (mfc_is_classic_sak(g_sak)) {
        // Prefer cached save (io=NULL) so user can save without card present
        bool ok = mfc_save_flipper_file(NULL, g_uid, g_uid_len, g_atqa, g_sak, dir, NULL, 0);
        if (!ok && g_pn532) {
            ESP_LOGW(TAG, "Offline save failed; retrying with live PN532");
            ok = mfc_save_flipper_file(g_pn532, g_uid, g_uid_len, g_atqa, g_sak, dir, NULL, 0);
        }
        if (!ok) {
            ESP_LOGE(TAG, "Failed to save Mifare Classic file");
            return false;
        }
        ESP_LOGI(TAG, "Mifare Classic file saved");
        return true;
    }

    // Non-Classic (NTAG/Ultralight) path
    const char *model_str = ntag_t2_model_str(g_model);
    snprintf(path, sizeof(path), "%s/%s_%s.nfc", dir, model_str, uid_part);

    // Determine total pages by model (NTAG/Ultralight)
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
        return false;
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
        uint32_t cv = 0; uint8_t tr = 0;
        esp_err_t erc = ntag2xx_read_counter(g_pn532, (uint8_t)ci, &cv);
        esp_err_t ert = ntag2xx_read_tearing(g_pn532, (uint8_t)ci, &tr);
        pos = 0; pos += snprintf(buf + pos, sizeof(buf) - pos, "Counter %d: %u\n", ci, (erc == ESP_OK) ? (unsigned)cv : 0);
        pos += snprintf(buf + pos, sizeof(buf) - pos, "Tearing %d: %02X\n", ci, (ert == ESP_OK) ? tr : 0);
        sd_card_append_file(path, buf, (size_t)pos);
    }

    // Read all pages and build page dump
    size_t cap = (size_t)pages_total * 48 + 64;
    char *pages = (char*)malloc(cap);
    if (!pages) {
        ESP_LOGE(TAG, "OOM building page dump");
        return false;
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
    return true;
#else
    ESP_LOGW(TAG, "NFC not enabled; nothing to save");
    return false;
#endif
}

static void create_nfc_scan_popup(void) {
    ESP_LOGI(TAG, "create_nfc_scan_popup");
    if (nfc_scan_popup && lv_obj_is_valid(nfc_scan_popup)) {
        cleanup_nfc_scan_popup(NULL);
    }
    if (!root || !lv_obj_is_valid(root)) return;
    // We'll reset the cancel flag right before (re)starting the scan task
    nfc_dict_skip_requested = false;
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

    // Progress indicators removed; we will update the title and details text instead

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
        ESP_LOGI(TAG, "create_nfc_scan_popup: starting scan task");
        nfc_scan_cancel = false;
        mfc_set_progress_callback(mfc_dict_progress_cb, NULL);
        xTaskCreate(nfc_scan_task, "nfc_scan", 6144, NULL, 5, &nfc_scan_task_handle);
    } else {
        // Previous task not fully exited yet; retry shortly
        ESP_LOGW(TAG, "create_nfc_scan_popup: previous scan task still running, scheduling retry");
        lv_timer_t *timer = lv_timer_create(nfc_try_start_scan_timer_cb, 120, NULL);
        (void)timer;
    }
#endif
}

#ifdef CONFIG_HAS_NFC
static void nfc_try_start_scan_timer_cb(lv_timer_t *t) {
    if (nfc_scan_task_handle == NULL) {
        ESP_LOGI(TAG, "nfc_try_start_scan_timer_cb: starting scan task after prior exit");
        nfc_scan_cancel = false;
        mfc_set_progress_callback(mfc_dict_progress_cb, NULL);
        xTaskCreate(nfc_scan_task, "nfc_scan", 6144, NULL, 5, &nfc_scan_task_handle);
        if (t) lv_timer_del(t);
    }
}
#endif

// Run heavy save on a worker task to avoid blocking LVGL thread
static void nfc_save_task(void *arg) {
    bool ok = write_flipper_nfc_file();
    // Notify UI on completion with result
    bool *res = (bool*)malloc(sizeof(bool));
    if (res) { *res = ok; lv_async_call(nfc_save_done_async, res); }
    else { lv_async_call(nfc_save_done_async, NULL); }
    nfc_save_in_progress = false;
    vTaskDelete(NULL);
}

static void nfc_save_done_async(void *ptr) {
    bool ok = (ptr != NULL) ? *((bool*)ptr) : false;
    if (nfc_title_label && lv_obj_is_valid(nfc_title_label)) {
        lv_label_set_text(nfc_title_label, ok ? "Saved!" : "Save failed");
    }
    if (nfc_scan_save_btn && lv_obj_is_valid(nfc_scan_save_btn)) {
        lv_obj_clear_state(nfc_scan_save_btn, LV_STATE_DISABLED);
    }
#ifdef CONFIG_HAS_NFC
    mfc_set_progress_callback(NULL, NULL);
#endif
    if (ptr) free(ptr);
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
            lv_obj_align(nfc_details_label, LV_ALIGN_TOP_MID, 0, 20);
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
    ESP_LOGI(TAG, "nfc_view_destroy");
    // Ensure any running scan is cancelled and resources are released
    cleanup_nfc_scan_popup(NULL); // sets nfc_scan_cancel=true
    nfc_scan_cancel = true;

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

#ifdef CONFIG_HAS_NFC
    // If scan task already exited, release PN532 here as a safety net
    if (nfc_scan_task_handle == NULL && g_pn532) {
        pn532_release(g_pn532);
        pn532_delete_driver(g_pn532);
        g_pn532 = NULL;
    }
    if (nfc_details_text) { free(nfc_details_text); nfc_details_text = NULL; }
    nfc_details_ready = false;
    nfc_details_visible = false;
#endif
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
