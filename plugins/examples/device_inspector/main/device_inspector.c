#include "../../../sdk/ghostesp_plugin_api.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

static const ghostesp_api_t *api;

typedef enum {
    PAGE_MENU,
    PAGE_SYSTEM,
    PAGE_WIFI,
    PAGE_BLE,
    PAGE_RGB,
    PAGE_STORAGE,
    PAGE_STORAGE_TEST,
    PAGE_GPS,
    PAGE_HARDWARE,
    PAGE_CANVAS,
    PAGE_INPUT,
    PAGE_THEME,
    PAGE_UNSAFE,
} page_id_t;

static page_id_t current_page;
static ghostesp_options_t main_menu;
static ghostesp_detail_t detail_view;
static ghostesp_popup_t popup;
static ghostesp_scan_t scan_status;
static ghostesp_ui_timer_t rgb_timer;
static int rgb_step;
static ghostesp_ui_obj_t canvas_obj;
static int canvas_tick;

static void show_menu(void);
static void open_page(page_id_t page);

#define ARRAY_COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

static void detail_back(void *user) {
    if (detail_view && api->ui_detail_destroy) {
        api->ui_detail_destroy(detail_view);
        detail_view = NULL;
    }
    show_menu();
}

static void popup_close(void *user) {
    if (popup && api->ui_popup_hide) api->ui_popup_hide(popup);
    if (popup && api->ui_popup_destroy) {
        api->ui_popup_destroy(popup);
        popup = NULL;
    }
}

static void show_popup(const char *title, const char *body) {
    if (!api->ui_popup_create) return;
    popup = api->ui_popup_create(260, 180);
    if (!popup) return;
    api->ui_popup_set_title(popup, title);
    api->ui_popup_set_body(popup, body);
    api->ui_popup_add_button(popup, "OK", popup_close, NULL);
    api->ui_popup_show(popup);
}

static void menu_select(void *user) {
    int idx = (int)(intptr_t)user;
    page_id_t pages[] = {
        PAGE_SYSTEM, PAGE_WIFI, PAGE_BLE, PAGE_RGB,
        PAGE_STORAGE, PAGE_STORAGE_TEST, PAGE_GPS,
        PAGE_HARDWARE, PAGE_CANVAS, PAGE_INPUT, PAGE_THEME, PAGE_UNSAFE,
    };
    if (idx >= 0 && idx < ARRAY_COUNT(pages)) {
        open_page(pages[idx]);
    }
}

static void show_menu(void) {
    current_page = PAGE_MENU;
    if (!api->ui_options_create) return;
    if (main_menu && api->ui_options_destroy) {
        api->ui_options_destroy(main_menu);
        main_menu = NULL;
    }
    main_menu = api->ui_options_create("Device Inspector");
    if (!main_menu) return;

    api->ui_options_add_item(main_menu, "System Info", menu_select, (void *)(intptr_t)0);
    api->ui_options_add_item(main_menu, "WiFi Scan", menu_select, (void *)(intptr_t)1);
    api->ui_options_add_item(main_menu, "BLE Scan", menu_select, (void *)(intptr_t)2);
    api->ui_options_add_item(main_menu, "RGB Test", menu_select, (void *)(intptr_t)3);
    api->ui_options_add_item(main_menu, "Storage Browser", menu_select, (void *)(intptr_t)4);
    api->ui_options_add_item(main_menu, "Storage R/W Test", menu_select, (void *)(intptr_t)5);
    api->ui_options_add_item(main_menu, "GPS Status", menu_select, (void *)(intptr_t)6);
    api->ui_options_add_item(main_menu, "Hardware Info", menu_select, (void *)(intptr_t)7);
    api->ui_options_add_item(main_menu, "Canvas Demo", menu_select, (void *)(intptr_t)8);
    api->ui_options_add_item(main_menu, "Input Tester", menu_select, (void *)(intptr_t)9);
    api->ui_options_add_item(main_menu, "Theme Colors", menu_select, (void *)(intptr_t)10);
    api->ui_options_add_item(main_menu, "Unsafe Probe", menu_select, (void *)(intptr_t)11);
    api->ui_options_add_back(main_menu, NULL, NULL);
}

static void open_system(void) {
    current_page = PAGE_SYSTEM;
    if (!api->ui_detail_create) return;
    detail_view = api->ui_detail_create("System Info");
    if (!detail_view) return;

    char buf[64];
    const char *ver = api->system_firmware_version ? api->system_firmware_version() : "unknown";
    api->ui_detail_add_info(detail_view, "Firmware", ver ? ver : "unknown");
    api->ui_detail_add_info(detail_view, "Target", api->target ? api->target : "unknown");

    snprintf(buf, sizeof(buf), "%lu ms", (unsigned long)api->system_uptime_ms());
    api->ui_detail_add_info(detail_view, "Uptime", buf);

    snprintf(buf, sizeof(buf), "%lu bytes", (unsigned long)api->system_free_heap());
    api->ui_detail_add_info(detail_view, "Free Heap", buf);

    snprintf(buf, sizeof(buf), "%lu bytes", (unsigned long)api->system_free_internal_heap());
    api->ui_detail_add_info(detail_view, "Free Internal", buf);

    size_t used = api->app_memory_used ? api->app_memory_used() : 0;
    size_t limit = api->app_memory_limit ? api->app_memory_limit() : 0;
    snprintf(buf, sizeof(buf), "%lu / %lu", (unsigned long)used, (unsigned long)limit);
    api->ui_detail_add_info(detail_view, "App Memory", buf);

    const char *aid = api->app_id ? api->app_id() : "unknown";
    api->ui_detail_add_info(detail_view, "App ID", aid);

    const char *datapath = api->app_data_path ? api->app_data_path() : "unknown";
    api->ui_detail_add_info(detail_view, "Data Path", datapath);

    snprintf(buf, sizeof(buf), "%ld x %ld",
             (long)(api->ui_screen_get_width ? api->ui_screen_get_width() : 0),
             (long)(api->ui_screen_get_height ? api->ui_screen_get_height() : 0));
    api->ui_detail_add_info(detail_view, "Screen", buf);

    uint32_t flags = api->flags;
    snprintf(buf, sizeof(buf), "0x%08lX", (unsigned long)flags);
    api->ui_detail_add_info(detail_view, "Flags", buf);

    const char *devname = api->settings_get_device_name ? api->settings_get_device_name() : NULL;
    if (devname) api->ui_detail_add_info(detail_view, "Device Name", devname);

    uint8_t theme = api->settings_get_theme ? api->settings_get_theme() : 0;
    snprintf(buf, sizeof(buf), "%u", (unsigned)theme);
    api->ui_detail_add_info(detail_view, "Theme", buf);

    api->ui_detail_add_divider(detail_view);
    api->ui_detail_add_back(detail_view, detail_back, NULL);
}

static void wifi_scan_done(void *user) {
    if (scan_status && api->ui_scan_status_close) {
        api->ui_scan_status_close(scan_status);
        scan_status = NULL;
    }
    if (!api->ui_detail_create) return;
    detail_view = api->ui_detail_create("WiFi Scan Results");
    if (!detail_view) return;

    char buf[96];
    uint16_t count = api->wifi_ap_count ? api->wifi_ap_count() : 0;
    snprintf(buf, sizeof(buf), "%u APs found", (unsigned)count);
    api->ui_detail_add_header(detail_view, buf);
    api->ui_detail_add_divider(detail_view);

    int show = count > 20 ? 20 : (int)count;
    for (int i = 0; i < show; i++) {
        ghostesp_wifi_ap_info_t ap;
        if (!api->wifi_scan_get_ap(i, &ap)) continue;
        snprintf(buf, sizeof(buf), "%s ch%d %ddBm", ap.ssid[0] ? ap.ssid : "(hidden)", ap.channel, ap.rssi);
        api->ui_detail_add_info(detail_view, "", buf);
    }
    if ((int)count > show) {
        snprintf(buf, sizeof(buf), "... and %u more", (unsigned)(count - show));
        api->ui_detail_add_info(detail_view, "", buf);
    }

    api->ui_detail_add_divider(detail_view);
    api->ui_detail_add_back(detail_view, detail_back, NULL);
}

static void open_wifi(void) {
    current_page = PAGE_WIFI;
    if (api->ui_scan_status_create) {
        scan_status = api->ui_scan_status_create("Scanning WiFi...");
        if (scan_status) api->ui_scan_status_set_progress(scan_status, 0, 1);
    }
    if (api->wifi_start_scan) api->wifi_start_scan();
    if (api->delay_ms) api->delay_ms(3000);
    wifi_scan_done(NULL);
}

static void ble_scan_done(void *user) {
    if (scan_status && api->ui_scan_status_close) {
        api->ui_scan_status_close(scan_status);
        scan_status = NULL;
    }
    if (!api->ui_detail_create) return;
    detail_view = api->ui_detail_create("BLE Scan Results");
    if (!detail_view) return;

    char buf[96];
    int count = api->ble_device_count ? api->ble_device_count() : 0;
    snprintf(buf, sizeof(buf), "%d devices found", count);
    api->ui_detail_add_header(detail_view, buf);
    api->ui_detail_add_divider(detail_view);

    int show = count > 15 ? 15 : count;
    for (int i = 0; i < show; i++) {
        ghostesp_ble_device_info_t dev;
        if (!api->ble_get_device(i, &dev)) continue;
        snprintf(buf, sizeof(buf), "%s %ddBm", dev.name[0] ? dev.name : "Unknown", dev.rssi);
        char mac[24];
        snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
                 dev.mac[0], dev.mac[1], dev.mac[2], dev.mac[3], dev.mac[4], dev.mac[5]);
        api->ui_detail_add_info(detail_view, mac, buf);
    }
    if (count > show) {
        snprintf(buf, sizeof(buf), "... and %d more", count - show);
        api->ui_detail_add_info(detail_view, "", buf);
    }

    api->ui_detail_add_divider(detail_view);
    api->ui_detail_add_back(detail_view, detail_back, NULL);
}

static void open_ble(void) {
    current_page = PAGE_BLE;
    if (api->ui_scan_status_create) {
        scan_status = api->ui_scan_status_create("Scanning BLE...");
        if (scan_status) api->ui_scan_status_set_progress(scan_status, 0, 1);
    }
    if (api->ble_start_scan) api->ble_start_scan();
    if (api->delay_ms) api->delay_ms(4000);
    if (api->ble_stop_scan) api->ble_stop_scan();
    ble_scan_done(NULL);
}

static void rgb_timer_cb(void *user) {
    if (current_page != PAGE_RGB || !api->rgb_set_all) return;
    rgb_step++;
    uint8_t r = 0, g = 0, b = 0;
    switch (rgb_step % 8) {
        case 0: r = 255; break;
        case 1: g = 255; break;
        case 2: b = 255; break;
        case 3: r = 255; g = 255; break;
        case 4: g = 255; b = 255; break;
        case 5: r = 255; b = 255; break;
        case 6: r = 255; g = 255; b = 255; break;
        case 7: r = 0; g = 0; b = 0; break;
    }
    api->rgb_set_all(r, g, b);

    char msg[64];
    snprintf(msg, sizeof(msg), "Step %d: RGB(%d,%d,%d)", rgb_step % 8, r, g, b);
    if (api->ui_set_status) api->ui_set_status(msg);
}

static void rgb_stop(void *user) {
    if (rgb_timer && api->ui_timer_delete) {
        api->ui_timer_delete(rgb_timer);
        rgb_timer = NULL;
    }
    if (api->rgb_set_all) api->rgb_set_all(0, 0, 0);
    detail_back(NULL);
}

static void open_rgb(void) {
    current_page = PAGE_RGB;
    rgb_step = 0;
    if (!api->ui_detail_create) return;
    detail_view = api->ui_detail_create("RGB Test");
    if (!detail_view) return;

    api->ui_detail_add_header(detail_view, "Cycles through 8 colors automatically");
    api->ui_detail_add_divider(detail_view);

    char hex[16];
    uint32_t colors[] = {0xFF0000, 0x00FF00, 0x0000FF, 0xFFFF00, 0x00FFFF, 0xFF00FF, 0xFFFFFF, 0x000000};
    const char *names[] = {"Red", "Green", "Blue", "Yellow", "Cyan", "Magenta", "White", "Off"};
    for (int i = 0; i < 8; i++) {
        snprintf(hex, sizeof(hex), "%s (#%06lX)", names[i], (unsigned long)colors[i]);
        api->ui_detail_add_info(detail_view, "", hex);
    }

    api->ui_detail_add_divider(detail_view);
    api->ui_detail_add_action(detail_view, "Stop Test", rgb_stop, NULL);
    api->ui_detail_add_back(detail_view, detail_back, NULL);

    if (api->ui_timer_create) {
        rgb_timer = api->ui_timer_create(rgb_timer_cb, 500, NULL);
    }
}

static void open_storage(void) {
    current_page = PAGE_STORAGE;
    if (!api->ui_detail_create) return;
    detail_view = api->ui_detail_create("Storage Browser");
    if (!detail_view) return;

    ghostesp_storage_entry_t entries[32];
    int count = api->app_storage_list ? api->app_storage_list("", entries, 32) : -1;
    char buf[80];

    if (count < 0) {
        api->ui_detail_add_info(detail_view, "Status", "Failed to list");
    } else if (count == 0) {
        api->ui_detail_add_info(detail_view, "Status", "Empty directory");
    } else {
        snprintf(buf, sizeof(buf), "%d items in app dir", count);
        api->ui_detail_add_header(detail_view, buf);
        api->ui_detail_add_divider(detail_view);
        int show = count > 15 ? 15 : count;
        for (int i = 0; i < show; i++) {
            snprintf(buf, sizeof(buf), "%s%s", entries[i].is_directory ? "[D] " : "    ", entries[i].name);
            api->ui_detail_add_info(detail_view, "", buf);
        }
        if (count > show) {
            snprintf(buf, sizeof(buf), "... and %d more", count - show);
            api->ui_detail_add_info(detail_view, "", buf);
        }
    }

    api->ui_detail_add_divider(detail_view);
    api->ui_detail_add_back(detail_view, detail_back, NULL);
}

static void open_storage_test(void) {
    current_page = PAGE_STORAGE_TEST;
    if (!api->ui_detail_create) return;
    detail_view = api->ui_detail_create("Storage R/W Test");
    if (!detail_view) return;

    char buf[96];
    char readback[128] = {0};
    const char *path = "test_rw.txt";
    const char *payload = "GhostESP storage test OK";

    api->ui_detail_add_header(detail_view, "Write / Read / Delete cycle");
    api->ui_detail_add_divider(detail_view);

    bool ok = api->app_storage_write && api->app_storage_write(path, payload, strlen(payload));
    snprintf(buf, sizeof(buf), "Write: %s", ok ? "OK" : "FAIL");
    api->ui_detail_add_info(detail_view, "1. Write", ok ? "OK" : "FAIL");

    int n = api->app_storage_read ? api->app_storage_read(path, readback, sizeof(readback) - 1) : -1;
    if (n > 0) readback[n] = '\0';
    api->ui_detail_add_info(detail_view, "2. Read", n > 0 ? "OK" : "FAIL");
    if (n > 0) {
        api->ui_detail_add_info(detail_view, "   Content", readback);
    }

    bool match = (n > 0 && strncmp(readback, payload, strlen(payload)) == 0);
    api->ui_detail_add_info(detail_view, "3. Verify", match ? "MATCH" : "MISMATCH");

    bool appended = api->app_storage_append && api->app_storage_append(path, " + appended", 10);
    api->ui_detail_add_info(detail_view, "4. Append", appended ? "OK" : "FAIL");

    bool exists = api->app_storage_exists && api->app_storage_exists(path);
    api->ui_detail_add_info(detail_view, "5. Exists", exists ? "YES" : "NO");

    bool deleted = api->app_storage_delete && api->app_storage_delete(path);
    api->ui_detail_add_info(detail_view, "6. Delete", deleted ? "OK" : "FAIL");

    bool gone = api->app_storage_exists && !api->app_storage_exists(path);
    api->ui_detail_add_info(detail_view, "7. Gone", gone ? "YES" : "NO - still there!");

    api->ui_detail_add_divider(detail_view);
    api->ui_detail_add_back(detail_view, detail_back, NULL);
}

static void open_gps(void) {
    current_page = PAGE_GPS;
    if (!api->ui_detail_create) return;
    detail_view = api->ui_detail_create("GPS Status");
    if (!detail_view) return;

    bool avail = api->gps_is_available && api->gps_is_available();
    api->ui_detail_add_info(detail_view, "Available", avail ? "YES" : "NO");

    if (avail) {
        bool fix = api->gps_has_fix && api->gps_has_fix();
        api->ui_detail_add_info(detail_view, "Fix", fix ? "YES" : "NO");

        char buf[64];
        snprintf(buf, sizeof(buf), "%.6f", api->gps_get_latitude ? api->gps_get_latitude() : 0.0);
        api->ui_detail_add_info(detail_view, "Latitude", buf);

        snprintf(buf, sizeof(buf), "%.6f", api->gps_get_longitude ? api->gps_get_longitude() : 0.0);
        api->ui_detail_add_info(detail_view, "Longitude", buf);

        snprintf(buf, sizeof(buf), "%.1f m", api->gps_get_altitude ? api->gps_get_altitude() : 0.0);
        api->ui_detail_add_info(detail_view, "Altitude", buf);

        snprintf(buf, sizeof(buf), "%d", api->gps_get_satellites ? api->gps_get_satellites() : 0);
        api->ui_detail_add_info(detail_view, "Satellites", buf);

        snprintf(buf, sizeof(buf), "%.1f km/h", api->gps_get_speed ? api->gps_get_speed() : 0.0f);
        api->ui_detail_add_info(detail_view, "Speed", buf);

        snprintf(buf, sizeof(buf), "%.1f deg", api->gps_get_heading ? api->gps_get_heading() : 0.0f);
        api->ui_detail_add_info(detail_view, "Heading", buf);
    } else {
        api->ui_detail_add_info(detail_view, "", "No GPS hardware detected");
    }

    api->ui_detail_add_divider(detail_view);
    api->ui_detail_add_back(detail_view, detail_back, NULL);
}

static void open_hardware(void) {
    current_page = PAGE_HARDWARE;
    if (!api->ui_detail_create) return;
    detail_view = api->ui_detail_create("Hardware Info");
    if (!detail_view) return;

    char buf[64];

    api->ui_detail_add_header(detail_view, "Radio Hardware");
    api->ui_detail_add_info(detail_view, "WiFi", api->wifi_start_scan ? "Present" : "N/A");
    api->ui_detail_add_info(detail_view, "BLE", api->ble_start_scan ? "Present" : "N/A");
    api->ui_detail_add_info(detail_view, "NFC", api->nfc_is_available && api->nfc_is_available() ? "Present" : "N/A");
    api->ui_detail_add_info(detail_view, "SubGHz", api->subghz_is_available && api->subghz_is_available() ? "Present" : "N/A");
    api->ui_detail_add_info(detail_view, "IR", api->ir_send_file ? "Present" : "N/A");
    api->ui_detail_add_info(detail_view, "BadUSB", api->badusb_run_script ? "Present" : "N/A");
    api->ui_detail_add_info(detail_view, "RGB LED", api->rgb_set_all ? "Present" : "N/A");
    api->ui_detail_add_info(detail_view, "GPS", api->gps_is_available && api->gps_is_available() ? "Present" : "N/A");

    api->ui_detail_add_divider(detail_view);
    api->ui_detail_add_header(detail_view, "Display");
    int32_t w = api->ui_screen_get_width ? api->ui_screen_get_width() : 0;
    int32_t h = api->ui_screen_get_height ? api->ui_screen_get_height() : 0;
    snprintf(buf, sizeof(buf), "%ld x %ld px", (long)w, (long)h);
    api->ui_detail_add_info(detail_view, "Resolution", buf);

    if (api->ui_theme_is_bright) {
        api->ui_detail_add_info(detail_view, "Theme Mode", api->ui_theme_is_bright() ? "Bright" : "Dark");
    }

    api->ui_detail_add_divider(detail_view);
    api->ui_detail_add_header(detail_view, "Memory");
    snprintf(buf, sizeof(buf), "%lu bytes free", (unsigned long)(api->system_free_heap ? api->system_free_heap() : 0));
    api->ui_detail_add_info(detail_view, "Heap", buf);
    snprintf(buf, sizeof(buf), "%lu bytes free", (unsigned long)(api->system_free_internal_heap ? api->system_free_internal_heap() : 0));
    api->ui_detail_add_info(detail_view, "Internal", buf);

    api->ui_detail_add_divider(detail_view);
    api->ui_detail_add_back(detail_view, detail_back, NULL);
}

static void canvas_tick_cb(void *user) {
    if (current_page != PAGE_CANVAS || !canvas_obj) return;
    canvas_tick++;
    int32_t w = api->ui_obj_get_width ? api->ui_obj_get_width(canvas_obj) : 200;
    int32_t h = api->ui_obj_get_height ? api->ui_obj_get_height(canvas_obj) : 100;

    api->ui_canvas_fill(canvas_obj, api->ui_theme_get_background ? api->ui_theme_get_background() : 0x000000);

    uint32_t accent = api->ui_theme_get_accent ? api->ui_theme_get_accent() : 0x1976D2;
    uint32_t text = api->ui_theme_get_text ? api->ui_theme_get_text() : 0xFFFFFF;
    uint32_t surface = api->ui_theme_get_surface ? api->ui_theme_get_surface() : 0x1A1A1A;

    int cx = w / 2;
    int cy = h / 2;
    int max_r = (w < h ? w : h) / 2 - 10;
    if (max_r < 10) max_r = 10;
    int radius = max_r * ((canvas_tick % 60) + 1) / 60;

    api->ui_canvas_draw_arc(canvas_obj, cx, cy, radius > 5 ? radius : 5, 0, 360, accent, 2);

    int bars = 8;
    int bar_w = (w - 20) / bars;
    for (int i = 0; i < bars; i++) {
        int bh = ((canvas_tick * 3 + i * 37) % (h - 20)) + 5;
        uint32_t c = (i % 2 == 0) ? accent : surface;
        api->ui_canvas_draw_rect(canvas_obj, 10 + i * bar_w, h - bh - 5, bar_w - 2, bh, c);
    }

    int dots = 6;
    for (int i = 0; i < dots; i++) {
        int angle = (canvas_tick * 5 + i * 60) % 360;
        float a = angle * 3.14159f / 180.0f;
        int px = (int)(cx + (float)max_r * 0.7f * cosf(a));
        int py = (int)(cy + (float)max_r * 0.7f * sinf(a));
        api->ui_canvas_draw_rect(canvas_obj, px - 2, py - 2, 5, 5, text);
    }
}

static void canvas_stop(void *user) {
    if (rgb_timer && api->ui_timer_delete) {
        api->ui_timer_delete(rgb_timer);
        rgb_timer = NULL;
    }
    canvas_obj = NULL;
    detail_back(NULL);
}

static void open_canvas(void) {
    current_page = PAGE_CANVAS;
    canvas_tick = 0;
    if (!api->ui_detail_create) return;
    detail_view = api->ui_detail_create("Canvas Demo");
    if (!detail_view) return;

    api->ui_detail_add_header(detail_view, "Animated drawing on canvas widget");
    api->ui_detail_add_divider(detail_view);

    if (api->ui_canvas_create) {
        int32_t sw = api->ui_screen_get_width ? api->ui_screen_get_width() : 240;
        int32_t ch = 100;
        canvas_obj = api->ui_canvas_create(NULL, sw - 20, ch);
    }

    api->ui_detail_add_divider(detail_view);
    api->ui_detail_add_action(detail_view, "Stop", canvas_stop, NULL);
    api->ui_detail_add_back(detail_view, detail_back, NULL);

    if (api->ui_timer_create) {
        rgb_timer = api->ui_timer_create(canvas_tick_cb, 50, NULL);
    }
}

static void open_input(void) {
    current_page = PAGE_INPUT;
    if (!api->ui_detail_create) return;
    detail_view = api->ui_detail_create("Input Tester");
    if (!detail_view) return;

    api->ui_detail_add_header(detail_view, "Press any button to see events");
    api->ui_detail_add_divider(detail_view);
    api->ui_detail_add_info(detail_view, "", "Waiting for input...");
    api->ui_detail_add_divider(detail_view);
    api->ui_detail_add_back(detail_view, detail_back, NULL);
}

static void open_theme(void) {
    current_page = PAGE_THEME;
    if (!api->ui_detail_create) return;
    detail_view = api->ui_detail_create("Theme Colors");
    if (!detail_view) return;

    char buf[16];
    uint32_t colors[] = {
        api->ui_theme_get_background ? api->ui_theme_get_background() : 0,
        api->ui_theme_get_surface ? api->ui_theme_get_surface() : 0,
        api->ui_theme_get_surface_alt ? api->ui_theme_get_surface_alt() : 0,
        api->ui_theme_get_text ? api->ui_theme_get_text() : 0,
        api->ui_theme_get_text_muted ? api->ui_theme_get_text_muted() : 0,
        api->ui_theme_get_accent ? api->ui_theme_get_accent() : 0,
    };
    const char *names[] = {"Background", "Surface", "Surface Alt", "Text", "Text Muted", "Accent"};

    api->ui_detail_add_header(detail_view, "Current theme palette");
    api->ui_detail_add_divider(detail_view);
    for (int i = 0; i < 6; i++) {
        snprintf(buf, sizeof(buf), "#%06lX", (unsigned long)colors[i]);
        api->ui_detail_add_info(detail_view, names[i], buf);
    }

    if (api->ui_theme_is_bright) {
        api->ui_detail_add_info(detail_view, "Mode", api->ui_theme_is_bright() ? "Bright" : "Dark");
    }

    api->ui_detail_add_divider(detail_view);
    api->ui_detail_add_back(detail_view, detail_back, NULL);
}

static void open_unsafe(void) {
    current_page = PAGE_UNSAFE;
    if (!api->ui_detail_create) return;
    detail_view = api->ui_detail_create("Native Probe");
    if (!detail_view) return;

    char buf[64];
    api->ui_detail_add_header(detail_view, "Raw LVGL pointers");
    api->ui_detail_add_divider(detail_view);

    void *scr = api->lv_scr_act ? api->lv_scr_act() : NULL;
    snprintf(buf, sizeof(buf), "%p", scr);
    api->ui_detail_add_info(detail_view, "lv_scr_act", buf);

    void *view = api->display_get_current_view ? api->display_get_current_view() : NULL;
    snprintf(buf, sizeof(buf), "%p", view);
    api->ui_detail_add_info(detail_view, "Current View", buf);

    void *sym = api->raw_symbol ? api->raw_symbol("lv_scr_act") : NULL;
    snprintf(buf, sizeof(buf), "%p", sym);
    api->ui_detail_add_info(detail_view, "raw_symbol test", buf);

    api->ui_detail_add_divider(detail_view);
    api->ui_detail_add_back(detail_view, detail_back, NULL);
}

static void open_page(page_id_t page) {
    if (main_menu && api->ui_options_destroy) {
        api->ui_options_destroy(main_menu);
        main_menu = NULL;
    }
    switch (page) {
        case PAGE_SYSTEM: open_system(); break;
        case PAGE_WIFI: open_wifi(); break;
        case PAGE_BLE: open_ble(); break;
        case PAGE_RGB: open_rgb(); break;
        case PAGE_STORAGE: open_storage(); break;
        case PAGE_STORAGE_TEST: open_storage_test(); break;
        case PAGE_GPS: open_gps(); break;
        case PAGE_HARDWARE: open_hardware(); break;
        case PAGE_CANVAS: open_canvas(); break;
        case PAGE_INPUT: open_input(); break;
        case PAGE_THEME: open_theme(); break;
        case PAGE_UNSAFE: open_unsafe(); break;
        default: show_menu(); break;
    }
}

static void inspector_start(void) {
    api->log("device_inspector started");
    show_menu();
}

static void inspector_stop(void) {
    if (rgb_timer && api->ui_timer_delete) {
        api->ui_timer_delete(rgb_timer);
        rgb_timer = NULL;
    }
    if (api->rgb_set_all) api->rgb_set_all(0, 0, 0);
    if (detail_view && api->ui_detail_destroy) {
        api->ui_detail_destroy(detail_view);
        detail_view = NULL;
    }
    if (main_menu && api->ui_options_destroy) {
        api->ui_options_destroy(main_menu);
        main_menu = NULL;
    }
    if (popup && api->ui_popup_destroy) {
        api->ui_popup_destroy(popup);
        popup = NULL;
    }
    if (scan_status && api->ui_scan_status_close) {
        api->ui_scan_status_close(scan_status);
        scan_status = NULL;
    }
    canvas_obj = NULL;
    api->log("device_inspector stopped");
}

static void inspector_input(const ghostesp_input_event_t *event) {
    if (!event) return;

    if (current_page == PAGE_INPUT && detail_view && api->ui_detail_clear) {
        const char *type_name = "UNKNOWN";
        switch (event->type) {
            case GHOSTESP_INPUT_LEFT: type_name = "LEFT"; break;
            case GHOSTESP_INPUT_RIGHT: type_name = "RIGHT"; break;
            case GHOSTESP_INPUT_UP: type_name = "UP"; break;
            case GHOSTESP_INPUT_DOWN: type_name = "DOWN"; break;
            case GHOSTESP_INPUT_SELECT: type_name = "SELECT"; break;
            case GHOSTESP_INPUT_BACK: type_name = "BACK"; break;
            case GHOSTESP_INPUT_KEY: type_name = "KEY"; break;
            case GHOSTESP_INPUT_TOUCH: type_name = "TOUCH"; break;
            default: break;
        }

        api->ui_detail_clear(detail_view);
        api->ui_detail_add_header(detail_view, "Input Event Received");
        api->ui_detail_add_divider(detail_view);

        char buf[64];
        api->ui_detail_add_info(detail_view, "Type", type_name);

        snprintf(buf, sizeof(buf), "%ld", (long)event->value);
        api->ui_detail_add_info(detail_view, "Value", buf);

        snprintf(buf, sizeof(buf), "%ld, %ld", (long)event->x, (long)event->y);
        api->ui_detail_add_info(detail_view, "Position", buf);

        api->ui_detail_add_info(detail_view, "Pressed", event->pressed ? "YES" : "NO");

        api->ui_detail_add_divider(detail_view);
        api->ui_detail_add_back(detail_view, detail_back, NULL);

        if (event->type == GHOSTESP_INPUT_BACK) {
            detail_back(NULL);
            return;
        }
        return;
    }

    if (current_page == PAGE_MENU && main_menu) {
        if (event->type == GHOSTESP_INPUT_LEFT || event->type == GHOSTESP_INPUT_UP) {
            if (api->ui_options_move_selection) api->ui_options_move_selection(main_menu, -1);
            return;
        }
        if (event->type == GHOSTESP_INPUT_RIGHT || event->type == GHOSTESP_INPUT_DOWN) {
            if (api->ui_options_move_selection) api->ui_options_move_selection(main_menu, 1);
            return;
        }
        if (event->type == GHOSTESP_INPUT_SELECT) {
            int idx = api->ui_options_get_selected ? api->ui_options_get_selected(main_menu) : -1;
            menu_select((void *)(intptr_t)idx);
            return;
        }
    }

    if (detail_view) {
        if (event->type == GHOSTESP_INPUT_LEFT || event->type == GHOSTESP_INPUT_UP) {
            if (api->ui_detail_move_selection) api->ui_detail_move_selection(detail_view, -1);
            return;
        }
        if (event->type == GHOSTESP_INPUT_RIGHT || event->type == GHOSTESP_INPUT_DOWN) {
            if (api->ui_detail_move_selection) api->ui_detail_move_selection(detail_view, 1);
            return;
        }
        if (event->type == GHOSTESP_INPUT_SELECT) {
            int selected = api->ui_detail_get_selected ? api->ui_detail_get_selected(detail_view) : -1;
            int count = api->ui_detail_get_count ? api->ui_detail_get_count(detail_view) : 0;
            if (current_page == PAGE_RGB && selected == count - 2) {
                rgb_stop(NULL);
            } else if (current_page == PAGE_CANVAS && selected == count - 2) {
                canvas_stop(NULL);
            } else {
                detail_back(NULL);
            }
            return;
        }
    }

    if (event->type == GHOSTESP_INPUT_BACK) {
        if (rgb_timer && api->ui_timer_delete) {
            api->ui_timer_delete(rgb_timer);
            rgb_timer = NULL;
        }
        if (api->rgb_set_all) api->rgb_set_all(0, 0, 0);
        canvas_obj = NULL;
        if (detail_view && api->ui_detail_destroy) {
            api->ui_detail_destroy(detail_view);
            detail_view = NULL;
        }
        if (scan_status && api->ui_scan_status_close) {
            api->ui_scan_status_close(scan_status);
            scan_status = NULL;
        }
        show_menu();
    }
}

static const ghostesp_app_t app = {
    .api_version = GHOSTESP_APP_API_VERSION,
    .struct_size = GHOSTESP_APP_STRUCT_SIZE_V1,
    .id = "device_inspector",
    .name = "Device Inspector",
    .on_start = inspector_start,
    .on_stop = inspector_stop,
    .on_input = inspector_input,
};

const ghostesp_app_t *ghostesp_app_init(const ghostesp_api_t *host_api) {
    if (!host_api || host_api->api_version != GHOSTESP_APP_API_VERSION) return 0;
    if (host_api->struct_size < GHOSTESP_API_STRUCT_SIZE_V1) return 0;
    api = host_api;
    return &app;
}

void app_main(void) {}
