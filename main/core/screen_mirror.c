#include "core/screen_mirror.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"
#include <string.h>
#include <stdio.h>

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG_ENABLED
#include "driver/usb_serial_jtag.h"
#define MIRROR_USE_JTAG 1
#else
#define MIRROR_USE_JTAG 0
#endif

static const char *TAG = "ScreenMirror";
static bool s_mirror_enabled = false;
static uint32_t s_frame_count = 0;

static void mirror_write(const void *data, size_t len) {
#if MIRROR_USE_JTAG
    const uint8_t *p = (const uint8_t *)data;
    size_t remaining = len;
    int64_t start = esp_timer_get_time();
    while (remaining > 0) {
        if (esp_timer_get_time() - start > 500000) break;  // 500ms max
        size_t chunk = (remaining > 64) ? 64 : remaining;
        int written = usb_serial_jtag_write_bytes(p, chunk, pdMS_TO_TICKS(50));
        if (written > 0) {
            p += written;
            remaining -= written;
        }
    }
#else
    fwrite(data, 1, len, stdout);
    fflush(stdout);
#endif
}

void screen_mirror_init(void) {
    s_mirror_enabled = false;
    s_frame_count = 0;
    ESP_LOGI(TAG, "Screen mirror initialized");
}

void screen_mirror_set_enabled(bool enabled) {
    s_mirror_enabled = enabled;
    if (enabled) {
        s_frame_count = 0;
        screen_mirror_send_info();
        lv_obj_invalidate(lv_scr_act());
        ESP_LOGI(TAG, "Screen mirror enabled");
    } else {
        ESP_LOGI(TAG, "Screen mirror disabled");
    }
}

bool screen_mirror_is_enabled(void) {
    return s_mirror_enabled;
}

void screen_mirror_refresh(void) {
    if (s_mirror_enabled) {
        lv_obj_invalidate(lv_scr_act());
    }
}

void screen_mirror_send_info(void) {
    if (!s_mirror_enabled) return;
    
    lv_disp_t *disp = lv_disp_get_default();
    if (!disp) return;
    
    uint16_t w = lv_disp_get_hor_res(disp);
    uint16_t h = lv_disp_get_ver_res(disp);
    
    mirror_packet_header_t hdr = {
        .marker = MIRROR_MARKER,
        .cmd = MIRROR_CMD_INFO,
        .x1 = w,
        .y1 = h,
        .x2 = 16,
        .y2 = 0,
        .data_len = 0
    };
    
    mirror_write(&hdr, sizeof(hdr));
}

void screen_mirror_send_area(const lv_area_t *area, lv_color_t *color_p) {
    if (!s_mirror_enabled || !area || !color_p) return;
    
    uint16_t w = area->x2 - area->x1 + 1;
    uint16_t h = area->y2 - area->y1 + 1;
    uint32_t pixel_count = w * h;
    uint32_t data_size = pixel_count * 2;
    
    mirror_packet_header_t hdr = {
        .marker = MIRROR_MARKER,
        .cmd = MIRROR_CMD_FRAME,
        .x1 = area->x1,
        .y1 = area->y1,
        .x2 = area->x2,
        .y2 = area->y2,
        .data_len = data_size
    };
    
    mirror_write(&hdr, sizeof(hdr));
    mirror_write(color_p, data_size);
    uint32_t end_marker = MIRROR_END_MARKER;
    mirror_write(&end_marker, sizeof(end_marker));
    
    s_frame_count++;
}
