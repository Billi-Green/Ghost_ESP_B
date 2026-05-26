#include "../../../sdk/ghostesp_plugin_api.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MAX_DISPLAY 8
#define RESULT_ROWS 3
#define SCAN_WAIT_TICKS 6
#define SCAN_DWELL_MS 3600

static const uint8_t TARGET_OUIS[][3] = {
    {0x24, 0x0A, 0xC4},
    {0x7C, 0xDF, 0xA1},
    {0xAC, 0x15, 0x18},
    {0x68, 0xC6, 0x3A},
    {0xF4, 0xCF, 0xA2},
    {0xDC, 0xDA, 0x0C},
    {0x10, 0x52, 0x1C},
};
#define TARGET_OUI_COUNT (sizeof(TARGET_OUIS) / sizeof(TARGET_OUIS[0]))

static const ghostesp_api_t *api;

static ghostesp_ui_obj_t root;
static ghostesp_ui_obj_t radar_card;
static ghostesp_ui_obj_t canvas;
static ghostesp_ui_obj_t status_label;
static ghostesp_ui_obj_t list_title_label;
static ghostesp_ui_obj_t device_labels[RESULT_ROWS];
static ghostesp_ui_obj_t back_btn;

#define KEY_UP     17
#define KEY_DOWN   18
#define KEY_LEFT   19
#define KEY_RIGHT  20
#define KEY_ESC    27
#define KEY_ENTER  10
#define KEY_BS     8
#define KEY_DEL    127
static ghostesp_ui_timer_t scan_timer;
static ghostesp_ui_timer_t draw_timer;
static ghostesp_ui_timer_t blink_timer;
static ghostesp_task_t scan_task;
static ghostesp_wifi_ap_info_t matches[MAX_DISPLAY];
static bool match_is_esp[MAX_DISPLAY];
static bool match_is_ghost[MAX_DISPLAY];
static int match_count;
static int esp_count;
static int ghost_count;
static int blink_state;
static int scan_state;
static int scan_ticks;
static int prev_count;
static int canvas_tick;
static int scroll_offset;
static volatile bool scan_running;
static volatile bool scan_busy;

static bool bssid_matches_target(const uint8_t bssid[6]) {
    for (int i = 0; i < TARGET_OUI_COUNT; i++) {
        if (bssid[0] == TARGET_OUIS[i][0] && bssid[1] == TARGET_OUIS[i][1] && bssid[2] == TARGET_OUIS[i][2])
            return true;
    }
    return false;
}

static char lower_char(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
}

static bool contains_icase(const char *text, const char *needle) {
    if (!text || !needle || !needle[0]) return false;
    size_t text_len = strlen(text);
    size_t needle_len = strlen(needle);
    if (needle_len > text_len) return false;

    for (size_t i = 0; i <= text_len - needle_len; i++) {
        bool match = true;
        for (size_t j = 0; j < needle_len; j++) {
            if (lower_char(text[i + j]) != lower_char(needle[j])) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

static bool ssid_matches_ghostesp(const char *ssid) {
    return contains_icase(ssid, "ghostnet") || contains_icase(ssid, "ghostesp");
}

static void style_panel(ghostesp_ui_obj_t obj) {
    if (!obj) return;
    if (api->ui_obj_set_bg_color) api->ui_obj_set_bg_color(obj, api->ui_theme_get_surface ? api->ui_theme_get_surface() : 0x181818);
    if (api->ui_obj_set_border_color) api->ui_obj_set_border_color(obj, api->ui_theme_get_surface_alt ? api->ui_theme_get_surface_alt() : 0x303030);
    if (api->ui_obj_set_border_width) api->ui_obj_set_border_width(obj, 1);
    if (api->ui_obj_set_radius) api->ui_obj_set_radius(obj, 8);
    if (api->ui_obj_set_pad) api->ui_obj_set_pad(obj, 6, 6, 5, 5);
}

static void stop_blink(void) {
    if (blink_timer && api->ui_timer_delete) {
        api->ui_timer_delete(blink_timer);
        blink_timer = NULL;
    }
    blink_state = 0;
    if (api->rgb_set_all) api->rgb_set_all(0, 0, 0);
}

static void blink_cb(void *user) {
    (void)user;
    if (!api->rgb_set_all) return;
    blink_state = !blink_state;
    if (blink_state) api->rgb_set_all(0, 80, 255);
    else api->rgb_set_all(0, 0, 0);
}

static void on_back_clicked(void *user) {
    (void)user;
    if (api->app_exit) api->app_exit();
}

static void start_blink(void) {
    stop_blink();
    if (api->rgb_set_all) api->rgb_set_all(0, 80, 255);
    blink_state = 1;
    if (api->ui_timer_create) {
        blink_timer = api->ui_timer_create(blink_cb, 350, NULL);
    }
}

static void update_display(void) {
    if (!canvas || !api->ui_canvas_fill || !api->ui_canvas_draw_rect) return;
    canvas_tick++;

    int32_t cw = api->ui_obj_get_width ? api->ui_obj_get_width(canvas) : 200;
    int32_t ch = api->ui_obj_get_height ? api->ui_obj_get_height(canvas) : 60;
    if (cw < 20) cw = 200;
    if (ch < 20) ch = 60;

    uint32_t bg = api->ui_theme_get_background ? api->ui_theme_get_background() : 0x000000;
    uint32_t accent = api->ui_theme_get_accent ? api->ui_theme_get_accent() : 0x1976D2;
    uint32_t dim = api->ui_theme_get_text_muted ? api->ui_theme_get_text_muted() : 0x666666;
    uint32_t surface = api->ui_theme_get_surface ? api->ui_theme_get_surface() : 0x1A1A1A;
    uint32_t surface_alt = api->ui_theme_get_surface_alt ? api->ui_theme_get_surface_alt() : 0x242424;
    uint32_t text = api->ui_theme_get_text ? api->ui_theme_get_text() : 0xFFFFFF;
    uint32_t ghost = 0xB45CFF;
    uint32_t ok = 0x28D7B5;

    api->ui_canvas_fill(canvas, bg);
    api->ui_canvas_draw_rect(canvas, 0, 0, cw, ch, surface);
    for (int y = 18; y < ch - 8; y += 14) {
        api->ui_canvas_draw_rect(canvas, 8, y, cw - 16, 1, surface_alt);
    }
    for (int x = 18; x < cw - 8; x += 28) {
        api->ui_canvas_draw_rect(canvas, x, 12, 1, ch - 22, surface_alt);
    }

    if (match_count > 0) {
        int bar_w = (cw - 20) / match_count;
        if (bar_w < 6) bar_w = 6;
        int bar_area_h = ch - 18;

        for (int i = 0; i < match_count && i < MAX_DISPLAY; i++) {
            int rssi = matches[i].rssi;
            if (rssi < -100) rssi = -100;
            if (rssi > -20) rssi = -20;
            int bar_h = bar_area_h * (rssi + 100) / 80;
            if (bar_h < 4) bar_h = 4;
            if (bar_h > bar_area_h) bar_h = bar_area_h;

            int x = 10 + i * bar_w;
            int y = ch - 8 - bar_h;
            uint32_t col = match_is_ghost[i] ? ghost : (match_is_esp[i] ? ok : accent);
            if (rssi < -82) col = dim;
            api->ui_canvas_draw_rect(canvas, x, y, bar_w - 3, bar_h, col);
            api->ui_canvas_draw_rect(canvas, x, y, bar_w - 3, 2, text);
            api->ui_canvas_draw_rect(canvas, x, ch - 6, bar_w - 3, 2, col);
        }
    }

    int sweep_x = 8 + ((canvas_tick * 7) % (cw - 16));
    api->ui_canvas_draw_rect(canvas, sweep_x, 8, 2, ch - 14, scan_busy ? accent : dim);

    if (api->ui_label_set_text) {
        char buf[128];
        if (status_label) {
            snprintf(buf, sizeof(buf), "%s  %d hit%s  %d OUI  %d Ghost",
                     scan_busy ? "Scan" : "Idle", match_count, match_count == 1 ? "" : "s", esp_count, ghost_count);
            api->ui_label_set_text(status_label, buf);
        }
        if (list_title_label) {
            if (match_count > 0) {
                int end = scroll_offset + RESULT_ROWS;
                if (end > match_count) end = match_count;
                snprintf(buf, sizeof(buf), "Results %d-%d of %d", scroll_offset + 1, end, match_count);
            } else {
                snprintf(buf, sizeof(buf), "OUI + GhostNet watch");
            }
            api->ui_label_set_text(list_title_label, buf);
        }
        int max_scroll = match_count - RESULT_ROWS;
        if (max_scroll < 0) max_scroll = 0;
        if (scroll_offset > max_scroll) scroll_offset = max_scroll;
        if (scroll_offset < 0) scroll_offset = 0;
        int visible = match_count - scroll_offset;
        if (visible > RESULT_ROWS) visible = RESULT_ROWS;
        if (visible < 0) visible = 0;
        for (int i = 0; i < RESULT_ROWS; i++) {
            if (!device_labels[i]) continue;
            int idx = scroll_offset + i;
            if (idx < match_count) {
                const char *kind = match_is_ghost[idx] ? (match_is_esp[idx] ? "GhostESP + OUI" : "GhostESP") : "ESP OUI";
                const char *local = (matches[idx].bssid[0] & 0x02) ? " (rand MAC)" : "";
                snprintf(buf, sizeof(buf), "%s%s\n%d dBm  ch %u\n%.24s",
                         kind, local, matches[idx].rssi, (unsigned)matches[idx].channel,
                         matches[idx].ssid[0] ? matches[idx].ssid : "<hidden>");
                api->ui_label_set_text(device_labels[i], buf);
                if (api->ui_obj_set_text_color) api->ui_obj_set_text_color(device_labels[i], match_is_ghost[idx] ? ghost : ok);
            } else if (i == 0 && match_count == 0) {
                api->ui_label_set_text(device_labels[i], "Scanning for ESP32 OUI and GhostESP...");
                if (api->ui_obj_set_text_color) api->ui_obj_set_text_color(device_labels[i], dim);
            } else {
                api->ui_label_set_text(device_labels[i], "");
            }
        }
    }
}

static void collect_matches(void) {
    int total = api->wifi_ap_count();
    int found = 0;
    int found_esp = 0;
    int found_ghost = 0;
    ghostesp_wifi_ap_info_t next_matches[MAX_DISPLAY];
    bool next_is_esp[MAX_DISPLAY] = {0};
    bool next_is_ghost[MAX_DISPLAY] = {0};

    for (uint16_t i = 0; i < (uint16_t)total && found < MAX_DISPLAY; i++) {
        ghostesp_wifi_ap_info_t ap;
        if (!api->wifi_scan_get_ap(i, &ap)) continue;
        bool esp = bssid_matches_target(ap.bssid);
        bool ghost = ssid_matches_ghostesp(ap.ssid);
        if (!esp && !ghost) continue;
        next_matches[found] = ap;
        next_is_esp[found] = esp;
        next_is_ghost[found] = ghost;
        found++;
        if (esp) found_esp++;
        if (ghost) found_ghost++;
    }

    for (int i = 0; i < MAX_DISPLAY; i++) {
        if (i < found) matches[i] = next_matches[i];
        match_is_esp[i] = next_is_esp[i];
        match_is_ghost[i] = next_is_ghost[i];
    }
    match_count = found;
    esp_count = found_esp;
    ghost_count = found_ghost;

    if (match_count > 0 && prev_count == 0) {
        start_blink();
    } else if (match_count == 0 && prev_count > 0) {
        stop_blink();
    }
    prev_count = match_count;
}

static void scan_cb(void *user) {
    (void)user;
    if (!api->wifi_start_scan || !api->wifi_stop_scan || !api->wifi_ap_count || !api->wifi_scan_get_ap) {
        return;
    }

    if (scan_state == 0) {
        scan_busy = true;
        api->wifi_start_scan();
        scan_state = 1;
        scan_ticks = 0;
        return;
    }

    scan_ticks++;
    if (scan_ticks < SCAN_WAIT_TICKS) return;

    api->wifi_stop_scan();
    scan_state = 0;
    collect_matches();
    scan_busy = false;

    update_display();
}

static void scan_task_fn(void *user) {
    (void)user;
    if (!api->wifi_start_scan || !api->wifi_stop_scan || !api->wifi_ap_count || !api->wifi_scan_get_ap || !api->delay_ms) {
        scan_running = false;
        return;
    }

    while (scan_running) {
        scan_busy = true;
        api->wifi_start_scan();

        int waited = 0;
        while (scan_running && waited < SCAN_DWELL_MS) {
            api->delay_ms(200);
            waited += 200;
        }

        api->wifi_stop_scan();
        if (!scan_running) break;
        collect_matches();
        scan_busy = false;

        api->delay_ms(350);
    }
    scan_busy = false;
    scan_task = NULL;
}

static void draw_cb(void *user) {
    (void)user;
    update_display();
}

static void create_ui(void) {
    if (!api->ui_screen_create) return;

    root = api->ui_screen_create("ESP32 Finder");
    if (!root) return;

    if (api->ui_obj_set_flex_flow) api->ui_obj_set_flex_flow(root, GHOSTESP_FLEX_FLOW_COLUMN);
    if (api->ui_obj_set_flex_align) api->ui_obj_set_flex_align(root, GHOSTESP_FLEX_ALIGN_START, GHOSTESP_FLEX_ALIGN_START, GHOSTESP_FLEX_ALIGN_START);
    if (api->ui_obj_set_pad_row) api->ui_obj_set_pad_row(root, 4);

    radar_card = api->ui_card_create ? api->ui_card_create(root) : root;
    style_panel(radar_card);
    if (radar_card && radar_card != root) {
        if (api->ui_obj_set_flex_grow) api->ui_obj_set_flex_grow(radar_card, 1);
        if (api->ui_obj_set_flex_flow) api->ui_obj_set_flex_flow(radar_card, GHOSTESP_FLEX_FLOW_COLUMN);
        if (api->ui_obj_set_flex_align) api->ui_obj_set_flex_align(radar_card, GHOSTESP_FLEX_ALIGN_START, GHOSTESP_FLEX_ALIGN_START, GHOSTESP_FLEX_ALIGN_START);
        if (api->ui_obj_set_pad_row) api->ui_obj_set_pad_row(radar_card, 2);
    }

    if (api->ui_canvas_create && api->ui_canvas_fill) {
        int32_t sw = api->ui_screen_get_width ? api->ui_screen_get_width() : 240;
        if (sw < 40) sw = 240;
        canvas = api->ui_canvas_create(radar_card, sw - 36, 54);
        if (canvas) {
            uint32_t bg = api->ui_theme_get_background ? api->ui_theme_get_background() : 0x000000;
            api->ui_canvas_fill(canvas, bg);
        }
    }

    if (api->ui_label_create) {
        status_label = api->ui_label_create(radar_card, "Starting WiFi scan...");
        if (status_label && api->ui_obj_set_font) api->ui_obj_set_font(status_label, GHOSTESP_FONT_CAPTION);
        if (status_label && api->ui_obj_set_text_color) api->ui_obj_set_text_color(status_label, api->ui_theme_get_accent ? api->ui_theme_get_accent() : 0x1976D2);
        list_title_label = api->ui_label_create(radar_card, "OUI + GhostNet watch");
        if (list_title_label && api->ui_obj_set_font) api->ui_obj_set_font(list_title_label, GHOSTESP_FONT_CAPTION);
        if (list_title_label && api->ui_obj_set_text_color) api->ui_obj_set_text_color(list_title_label, api->ui_theme_get_text ? api->ui_theme_get_text() : 0xFFFFFF);
        device_labels[0] = api->ui_label_create(radar_card, "Scanning for ESP32 OUI and GhostESP...");
        for (int i = 1; i < RESULT_ROWS; i++) {
            device_labels[i] = api->ui_label_create(radar_card, "");
        }
        for (int i = 0; i < RESULT_ROWS; i++) {
            if (device_labels[i] && api->ui_obj_set_font) api->ui_obj_set_font(device_labels[i], GHOSTESP_FONT_CAPTION);
            if (device_labels[i] && api->ui_obj_set_text_color) api->ui_obj_set_text_color(device_labels[i], api->ui_theme_get_text_muted ? api->ui_theme_get_text_muted() : 0xD0D0D0);
        }
    }

    if (api->ui_button_create) {
        back_btn = api->ui_button_create(radar_card, "Back", on_back_clicked, NULL);
        if (back_btn && api->ui_obj_set_text_color) api->ui_obj_set_text_color(back_btn, api->ui_theme_get_text ? api->ui_theme_get_text() : 0xFFFFFF);
    }

    if (api->ui_timer_create) {
        draw_timer = api->ui_timer_create(draw_cb, 250, NULL);
    }
    scan_running = true;
    if (api->task_create) {
        scan_task = api->task_create("esp32_scan", scan_task_fn, NULL, 4096, 4);
    }
    if (!scan_task && api->ui_timer_create) {
        scan_timer = api->ui_timer_create(scan_cb, 600, NULL);
    }
    update_display();
}

static void destroy_ui(void) {
    if (scan_timer && api->ui_timer_delete) {
        api->ui_timer_delete(scan_timer);
        scan_timer = NULL;
    }
    if (draw_timer && api->ui_timer_delete) {
        api->ui_timer_delete(draw_timer);
        draw_timer = NULL;
    }
    scan_running = false;
    if (scan_task && api->task_delete) {
        api->task_delete(scan_task);
        scan_task = NULL;
    }
    stop_blink();
    if (api->wifi_stop_scan) api->wifi_stop_scan();
    if (root && api->ui_obj_delete) api->ui_obj_delete(root);
    root = NULL;
    radar_card = NULL;
    canvas = NULL;
    status_label = NULL;
    list_title_label = NULL;
    back_btn = NULL;
    for (int i = 0; i < RESULT_ROWS; i++) {
        device_labels[i] = NULL;
    }
}

static void esp32_finder_start(void) {
    if (api->log) api->log("ESP32 Finder started");
    match_count = 0;
    esp_count = 0;
    ghost_count = 0;
    prev_count = 0;
    scan_state = 0;
    scan_ticks = 0;
    canvas_tick = 0;
    scroll_offset = 0;
    scan_running = false;
    scan_busy = false;
    scan_task = NULL;
    create_ui();
}

static void esp32_finder_stop(void) {
    destroy_ui();
    if (api->log) api->log("ESP32 Finder stopped");
}

static void esp32_finder_input(const ghostesp_input_event_t *event) {
    if (!event) return;

    bool up = (event->type == GHOSTESP_INPUT_UP || event->type == GHOSTESP_INPUT_LEFT);
    bool down = (event->type == GHOSTESP_INPUT_DOWN || event->type == GHOSTESP_INPUT_RIGHT);

    if (event->type == GHOSTESP_INPUT_KEY) {
        int v = event->value;
        if (v == KEY_ESC || v == '`' || v == 'q' || v == 'Q' || v == KEY_BS || v == KEY_DEL || v == KEY_ENTER) {
            if (api->app_exit) api->app_exit();
            return;
        }
        if (v == KEY_UP || v == KEY_LEFT || v == 'w' || v == 'W' || v == 'a' || v == 'A') {
            up = true;
        } else if (v == KEY_DOWN || v == KEY_RIGHT || v == 's' || v == 'S' || v == 'd' || v == 'D') {
            down = true;
        }
    }

    if (event->type == GHOSTESP_INPUT_TOUCH && event->pressed) {
        int32_t sh = api->ui_screen_get_height ? api->ui_screen_get_height() : 135;
        if (event->y > sh - 28 && event->x > 160) {
            if (api->app_exit) api->app_exit();
            return;
        }
    }

    if (up) {
        if (scroll_offset > 0) {
            scroll_offset--;
            update_display();
        }
        return;
    }
    if (down) {
        int max_scroll = match_count - RESULT_ROWS;
        if (max_scroll < 0) max_scroll = 0;
        if (scroll_offset < max_scroll) {
            scroll_offset++;
            update_display();
        }
        return;
    }
    if (event->type == GHOSTESP_INPUT_BACK || event->type == GHOSTESP_INPUT_SELECT) {
        if (api->app_exit) api->app_exit();
    }
}

static const ghostesp_app_t app = GHOSTESP_APP_DEFINE(
    "esp32_finder",
    "ESP32 Finder",
    esp32_finder_start,
    esp32_finder_stop,
    esp32_finder_input,
    0
);

const ghostesp_app_t *ghostesp_app_init(const ghostesp_api_t *host_api) {
    if (!host_api || host_api->api_version != GHOSTESP_APP_API_VERSION) return 0;
    if (host_api->struct_size < GHOSTESP_API_STRUCT_SIZE_V1) return 0;
    api = host_api;
    return &app;
}

void app_main(void) {}
