#include "../../../sdk/ghostesp_plugin_api.h"

#include <stdio.h>

#define MAX_RESULTS 16
#define KEY_ESC 27
#define KEY_ENTER 10
#define KEY_BS 8
#define KEY_DEL 127

static const ghostesp_api_t *api;
static ghostesp_options_t menu;
static ghostesp_detail_t detail;
static ghostesp_scan_t scan_status;

static char s_app_id[] = "esp32_finder";
static char s_app_name[] = "ESP32 Finder";
static char s_log_started[] = "ESP32 Finder started";
static char s_log_stopped[] = "ESP32 Finder stopped";
static char s_scan_now[] = "Scan Now";
static char s_scan_again[] = "Scan Again";
static char s_status[] = "Status";
static char s_no_matches[] = "No ESP32/GhostESP matches";
static char s_hidden[] = "hidden";
static char s_summary[] = "Summary";
static char s_scan_title[] = "Scanning WiFi...";
static char s_empty[] = "";

static void show_menu(void);
static void run_scan(void);

static void navigate_back(void) {
    if (detail && api->ui_detail_destroy) {
        api->ui_detail_destroy(detail);
        detail = NULL;
        show_menu();
        return;
    }
    if (api->app_exit) api->app_exit();
}

static void touch_up_clicked(void *user) {
    (void)user;
    if (menu && api->ui_options_move_selection) api->ui_options_move_selection(menu, -1);
    else if (detail && api->ui_detail_step_up) api->ui_detail_step_up(detail);
}

static void touch_down_clicked(void *user) {
    (void)user;
    if (menu && api->ui_options_move_selection) api->ui_options_move_selection(menu, 1);
    else if (detail && api->ui_detail_step_down) api->ui_detail_step_down(detail);
}

static void destroy_views(void) {
    if (detail && api->ui_detail_destroy) {
        api->ui_detail_destroy(detail);
        detail = NULL;
    }
    if (scan_status && api->ui_scan_status_close) {
        api->ui_scan_status_close(scan_status);
        scan_status = NULL;
    }
    if (menu && api->ui_options_destroy) {
        api->ui_options_destroy(menu);
        menu = NULL;
    }
    if (api->rgb_set_all) api->rgb_set_all(0, 0, 0);
}

static void detail_back(void *user) {
    (void)user;
    navigate_back();
}

static void scan_again(void *user) {
    (void)user;
    run_scan();
}

static void run_scan(void) {
    if (menu && api->ui_options_destroy) {
        api->ui_options_destroy(menu);
        menu = NULL;
    }
    if (detail && api->ui_detail_destroy) {
        api->ui_detail_destroy(detail);
        detail = NULL;
    }
    if (api->ui_scan_status_create) {
        scan_status = api->ui_scan_status_create(s_scan_title);
        if (scan_status && api->ui_scan_status_set_progress) api->ui_scan_status_set_progress(scan_status, 0, 1);
    }
    if (api->wifi_start_scan) api->wifi_start_scan();
    if (api->delay_ms) api->delay_ms(3000);
    if (scan_status && api->ui_scan_status_close) {
        api->ui_scan_status_close(scan_status);
        scan_status = NULL;
    }

    if (!api->ui_detail_create) return;

    detail = api->ui_detail_create(s_app_name);
    if (!detail) return;

    int total = api->wifi_ap_count ? api->wifi_ap_count() : 0;
    char buf[96];
    snprintf(buf, sizeof(buf), "%u APs found", (unsigned)total);
    api->ui_detail_add_info(detail, s_summary, buf);

    int shown = total > MAX_RESULTS ? MAX_RESULTS : total;
    for (int i = 0; i < shown; i++) {
        ghostesp_wifi_ap_info_t ap;
        if (!api->wifi_scan_get_ap || !api->wifi_scan_get_ap((uint16_t)i, &ap)) continue;
        snprintf(buf, sizeof(buf), "%s ch%d %ddBm", ap.ssid[0] ? ap.ssid : s_hidden, ap.channel, ap.rssi);
        api->ui_detail_add_info(detail, s_empty, buf);
    }
    if (total > shown) {
        snprintf(buf, sizeof(buf), "... and %u more", (unsigned)(total - shown));
        api->ui_detail_add_info(detail, s_empty, buf);
    }
    if (shown == 0) api->ui_detail_add_info(detail, s_status, s_no_matches);
    api->ui_detail_add_action(detail, s_scan_again, scan_again, NULL);
    api->ui_detail_add_back(detail, detail_back, NULL);
}

static void scan_selected(void *user) {
    (void)user;
    run_scan();
}

static void exit_selected(void *user) {
    (void)user;
    if (api->app_exit) api->app_exit();
}

static void show_menu(void) {
    if (menu && api->ui_options_destroy) {
        api->ui_options_destroy(menu);
        menu = NULL;
    }
    if (!api->ui_options_create) return;
    menu = api->ui_options_create(s_app_name);
    if (!menu) return;
    api->ui_options_add_item(menu, s_scan_now, scan_selected, NULL);
    api->ui_options_add_back(menu, exit_selected, NULL);
}

static void esp32_finder_start(void) {
    if (api->log) api->log(s_log_started);
    show_menu();
}

static void esp32_finder_stop(void) {
    destroy_views();
    if (api->log) api->log(s_log_stopped);
}

static void esp32_finder_input(const ghostesp_input_event_t *event) {
    if (!event) return;
    if (event->type == GHOSTESP_INPUT_LEFT || event->type == GHOSTESP_INPUT_UP) {
        touch_up_clicked(NULL);
        return;
    }
    if (event->type == GHOSTESP_INPUT_RIGHT || event->type == GHOSTESP_INPUT_DOWN) {
        touch_down_clicked(NULL);
        return;
    }
    if (event->type == GHOSTESP_INPUT_SELECT) {
        if (menu && api->ui_options_get_selected) {
            int selected = api->ui_options_get_selected(menu);
            if (selected == 0) run_scan();
            else navigate_back();
        } else if (detail && api->ui_detail_activate_selected) {
            api->ui_detail_activate_selected(detail);
        }
        return;
    }
    if (event->type == GHOSTESP_INPUT_BACK) {
        navigate_back();
        return;
    }
    if (event->type == GHOSTESP_INPUT_KEY) {
        int v = event->value;
        if (v == KEY_ESC || v == KEY_BS || v == KEY_DEL || v == 'q' || v == 'Q') navigate_back();
        else if (v == KEY_ENTER || v == 'r' || v == 'R' || v == ' ') run_scan();
    }
}

static const ghostesp_app_t app = {
    .api_version = GHOSTESP_APP_API_VERSION,
    .struct_size = GHOSTESP_APP_STRUCT_SIZE_V1,
    .id = s_app_id,
    .name = s_app_name,
    .on_start = esp32_finder_start,
    .on_stop = esp32_finder_stop,
    .on_input = esp32_finder_input,
};

const ghostesp_app_t *ghostesp_app_init(const ghostesp_api_t *host_api) {
    if (!host_api || host_api->api_version != GHOSTESP_APP_API_VERSION) return 0;
    if (host_api->struct_size < GHOSTESP_API_STRUCT_SIZE_V1) return 0;
    api = host_api;
    return &app;
}

void app_main(void) {}
