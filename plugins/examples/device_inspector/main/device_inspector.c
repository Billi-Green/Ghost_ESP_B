#include "../../../sdk/ghostesp_plugin_api.h"

#include <stdio.h>
#include <string.h>

static const ghostesp_api_t *api;
static int selected;
static int screen;

static const char *items[] = {
    "System stats",
    "WiFi quick scan",
    "RGB test pulse",
    "Storage write/read",
    "Unsafe probe",
};

static void draw_menu(void) {
    api->ui_set_title("Device Inspector");
    api->ui_clear();
    api->ui_print("Native SD app test suite\n");
    api->ui_print("Use up/down and select. Back exits.\n\n");

    for (int i = 0; i < (int)(sizeof(items) / sizeof(items[0])); ++i) {
        api->ui_print(i == selected ? "> " : "  ");
        api->ui_print(items[i]);
        api->ui_print("\n");
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
    char line[128];
    api->ui_set_title("WiFi Scan");
    api->ui_clear();
    api->ui_print("Starting AP scan through host API...\n");
    api->wifi_start_scan();
    api->delay_ms(2500);
    snprintf(line, sizeof(line), "Known AP count: %u\n", (unsigned)api->wifi_ap_count());
    api->ui_print(line);
    api->ui_print("Open Terminal or native WiFi app for full results.\n");
    api->ui_print("\nSelect returns to menu.\n");
}

static void run_rgb_test(void) {
    api->ui_set_title("RGB Test");
    api->ui_clear();
    api->ui_print("Pulsing RGB through host API...\n");
    api->rgb_set_all(255, 0, 0);
    api->delay_ms(200);
    api->rgb_set_all(0, 255, 0);
    api->delay_ms(200);
    api->rgb_set_all(0, 0, 255);
    api->delay_ms(200);
    api->rgb_set_all(0, 0, 0);
    api->ui_print("Done.\n\nSelect returns to menu.\n");
}

static void run_storage_test(void) {
    const char *path = "/mnt/ghostesp/apps/device_inspector/last_run.txt";
    char line[160];
    char buf[96] = {0};
    snprintf(line, sizeof(line), "uptime_ms=%lu\nheap=%lu\n",
             (unsigned long)api->system_uptime_ms(),
             (unsigned long)api->system_free_heap());

    api->ui_set_title("Storage Test");
    api->ui_clear();
    if (api->storage_write(path, line, strlen(line))) {
        api->ui_print("Wrote last_run.txt\n");
    } else {
        api->ui_print("Write failed. Is SD mounted?\n");
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
        api->ui_print("Enable CONFIG_NATIVE_SD_APPS_UNSAFE_MODE and set unsafe=true.\n");
    } else {
        void *scr = api->unsafe->lv_scr_act();
        void *view = api->unsafe->display_get_current_view();
        snprintf(line, sizeof(line), "lv_scr_act: %p\ncurrent View: %p\n", scr, view);
        api->ui_print(line);
        api->ui_print("Raw pointers are intentionally dangerous.\n");
    }
    api->ui_print("\nSelect returns to menu.\n");
}

static void open_selected(void) {
    screen = selected + 1;
    switch (selected) {
        case 0: show_system_stats(); break;
        case 1: run_wifi_scan(); break;
        case 2: run_rgb_test(); break;
        case 3: run_storage_test(); break;
        case 4: run_unsafe_probe(); break;
        default: draw_menu(); break;
    }
}

static void inspector_start(void) {
    selected = 0;
    screen = 0;
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
        if (selected < 0) selected = (int)(sizeof(items) / sizeof(items[0])) - 1;
        draw_menu();
    } else if (event->type == GHOSTESP_INPUT_DOWN || event->type == GHOSTESP_INPUT_RIGHT) {
        selected++;
        if (selected >= (int)(sizeof(items) / sizeof(items[0]))) selected = 0;
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
