#include "../../../sdk/ghostesp_plugin_api.h"

#include <stdio.h>
#include <string.h>

static const ghostesp_api_t *api;
static int selected;
static int screen;
static int scroll;

#define VISIBLE_LINES 7
#define ARRAY_COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

static const char *menu_items[] = {
    "System stats",
    "WiFi scan",
    "RGB pulse",
    "Storage browser",
    "Storage write/read",
    "Unsafe probe",
};

static void draw_scrollable_list(const char *title, const char **items, int count, int sel, int off) {
    api->ui_set_title(title);
    api->ui_clear();
    if (off > 0) api->ui_print("^\n");
    for (int i = off; i < count && i < off + VISIBLE_LINES; i++) {
        api->ui_print(i == sel ? "> " : "  ");
        api->ui_print(items[i]);
        api->ui_print("\n");
    }
    if (off + VISIBLE_LINES < count) api->ui_print("v\n");
}

static void draw_menu(void) {
    draw_scrollable_list("Device Inspector", menu_items, ARRAY_COUNT(menu_items), selected, scroll);
}

static void adjust_scroll(void) {
    if (selected < scroll) scroll = selected;
    if (selected >= scroll + VISIBLE_LINES) scroll = selected - VISIBLE_LINES + 1;
    if (scroll < 0) scroll = 0;
    if (scroll > ARRAY_COUNT(menu_items) - VISIBLE_LINES) {
        scroll = ARRAY_COUNT(menu_items) - VISIBLE_LINES;
        if (scroll < 0) scroll = 0;
    }
}

static void show_system_stats(void) {
    char line[128];
    api->ui_set_title("System Stats");
    api->ui_clear();
    snprintf(line, sizeof(line), "Target: %s\n", api->target ? api->target : "unknown");
    api->ui_print(line);
    snprintf(line, sizeof(line), "Uptime: %lu ms\n", (unsigned long)api->system_uptime_ms());
    api->ui_print(line);
    snprintf(line, sizeof(line), "Free heap: %lu bytes\n", (unsigned long)api->system_free_heap());
    api->ui_print(line);
    snprintf(line, sizeof(line), "Free internal: %lu bytes\n", (unsigned long)api->system_free_internal_heap());
    api->ui_print(line);
    api->ui_print("\nSelect returns to menu.\n");
}

static void run_wifi_scan(void) {
    char line[160];
    api->ui_set_title("WiFi Scan");
    api->ui_clear();
    api->ui_print("Scanning...\n");
    api->wifi_start_scan();
    api->delay_ms(2500);
    uint16_t count = api->wifi_ap_count();
    snprintf(line, sizeof(line), "APs: %u\n", (unsigned)count);
    api->ui_print(line);
    for (uint16_t i = 0; i < count; i++) {
        ghostesp_wifi_ap_info_t ap;
        if (!api->wifi_scan_get_ap(i, &ap)) continue;
        if (i >= 6) {
            api->ui_print("...\n");
            break;
        }
        snprintf(line, sizeof(line), "%s ch%d %ddBm\n", ap.ssid, ap.channel, ap.rssi);
        api->ui_print(line);
    }
    api->ui_print("\nSelect returns to menu.\n");
}

static void run_rgb_test(void) {
    api->ui_set_title("RGB Test");
    api->ui_clear();
    api->ui_print("Pulsing...\n");
    api->rgb_set_all(255, 0, 0);
    api->delay_ms(200);
    api->rgb_set_all(0, 255, 0);
    api->delay_ms(200);
    api->rgb_set_all(0, 0, 255);
    api->delay_ms(200);
    api->rgb_set_all(0, 0, 0);
    api->ui_print("Done.\n\nSelect returns to menu.\n");
}

static void run_storage_browser(void) {
    const char *path = "/mnt/ghostesp/apps/device_inspector";
    ghostesp_storage_entry_t entries[32];
    char line[80];
    int count = api->storage_list(path, entries, 32);
    api->ui_set_title("Storage Browser");
    api->ui_clear();
    if (count < 0) {
        api->ui_print("List failed.\n");
    } else if (count == 0) {
        api->ui_print("(empty)\n");
    } else {
        snprintf(line, sizeof(line), "%d items in app dir:\n", count);
        api->ui_print(line);
        for (int i = 0; i < count && i < 6; i++) {
            snprintf(line, sizeof(line), "%s%s\n", entries[i].is_directory ? "[D] " : "[F] ", entries[i].name);
            api->ui_print(line);
        }
        if (count > 6) api->ui_print("...\n");
    }
    api->ui_print("\nSelect returns to menu.\n");
}

static void run_storage_test(void) {
    const char *path = "/mnt/ghostesp/apps/device_inspector/last_run.txt";
    char line[160];
    char buf[96] = {0};
    snprintf(line, sizeof(line), "uptime=%lu\nheap=%lu\n",
             (unsigned long)api->system_uptime_ms(),
             (unsigned long)api->system_free_heap());

    api->ui_set_title("Storage Test");
    api->ui_clear();
    if (api->storage_write(path, line, strlen(line))) {
        api->ui_print("Wrote last_run.txt\n");
    } else {
        api->ui_print("Write failed.\n");
    }
    if (api->storage_append(path, "appended\n", 9)) {
        api->ui_print("Appended.\n");
    }
    int n = api->storage_read(path, buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = 0;
        api->ui_print("Readback:\n");
        api->ui_print(buf);
    }
    api->ui_print("\nSelect returns to menu.\n");
}

static void run_unsafe_probe(void) {
    char line[160];
    api->ui_set_title("Unsafe Probe");
    api->ui_clear();
    if (!api->unsafe) {
        api->ui_print("Unsafe API not available.\n");
        api->ui_print("Enable unsafe mode in firmware.\n");
    } else {
        void *scr = api->unsafe->lv_scr_act();
        void *view = api->unsafe->display_get_current_view();
        snprintf(line, sizeof(line), "lv_scr_act: %p\ncurrent View: %p\n", scr, view);
        api->ui_print(line);
        api->ui_print("Raw pointers are dangerous.\n");
    }
    api->ui_print("\nSelect returns to menu.\n");
}

static void open_selected(void) {
    screen = selected + 1;
    switch (selected) {
        case 0: show_system_stats(); break;
        case 1: run_wifi_scan(); break;
        case 2: run_rgb_test(); break;
        case 3: run_storage_browser(); break;
        case 4: run_storage_test(); break;
        case 5: run_unsafe_probe(); break;
        default: draw_menu(); break;
    }
}

static void inspector_start(void) {
    selected = 0;
    screen = 0;
    scroll = 0;
    api->log("device_inspector started");
    draw_menu();
}

static void inspector_stop(void) {
    if (api) {
        api->rgb_set_all(0, 0, 0);
        api->log("device_inspector stopped");
    }
}

static void inspector_input(const ghostesp_input_event_t *event) {
    if (!event) return;
    if (screen != 0) {
        if (event->type == GHOSTESP_INPUT_SELECT || event->type == GHOSTESP_INPUT_BACK) {
            screen = 0;
            draw_menu();
        }
        return;
    }

    if (event->type == GHOSTESP_INPUT_UP || event->type == GHOSTESP_INPUT_LEFT) {
        selected--;
        if (selected < 0) selected = ARRAY_COUNT(menu_items) - 1;
        adjust_scroll();
        draw_menu();
    } else if (event->type == GHOSTESP_INPUT_DOWN || event->type == GHOSTESP_INPUT_RIGHT) {
        selected++;
        if (selected >= ARRAY_COUNT(menu_items)) selected = 0;
        adjust_scroll();
        draw_menu();
    } else if (event->type == GHOSTESP_INPUT_SELECT) {
        open_selected();
    }
}

static const ghostesp_app_t app = {
    .api_version = GHOSTESP_APP_API_VERSION,
    .id = "device_inspector",
    .name = "Device Inspector",
    .on_start = inspector_start,
    .on_stop = inspector_stop,
    .on_input = inspector_input,
};

const ghostesp_app_t *ghostesp_app_init(const ghostesp_api_t *host_api) {
    if (!host_api || host_api->api_version != GHOSTESP_APP_API_VERSION) return 0;
    api = host_api;
    return &app;
}

void app_main(void) {
}
