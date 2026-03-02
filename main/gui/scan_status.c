#include "gui/scan_status.h"
#include "managers/display_manager.h"
#include "managers/settings_manager.h"
#include "gui/theme_palette_api.h"
#include "lvgl.h"
#include "esp_timer.h"
#include <stdlib.h>
#include <string.h>

struct scan_status_t {
    lv_obj_t *container;
    lv_obj_t *spinner;
    lv_obj_t *label;
    lv_obj_t *progress_label;
    lv_timer_t *anim_timer;
    int anim_dots;
    char base_message[128];
    int current;
    int total;
    bool active;
};

static void anim_timer_cb(lv_timer_t *timer) {
    scan_status_t *ss = (scan_status_t *)timer->user_data;
    if (!ss || !ss->active || !ss->label) return;
    
    ss->anim_dots = (ss->anim_dots + 1) % 4;
    
    char buf[160];
    strcpy(buf, ss->base_message);
    for (int i = 0; i < ss->anim_dots; i++) {
        strcat(buf, ".");
    }
    lv_label_set_text(ss->label, buf);
}

static lv_coord_t get_screen_width(void) {
    lv_obj_t *scr = lv_scr_act();
    return scr ? lv_obj_get_width(scr) : 240;
}

static lv_coord_t get_screen_height(void) {
    lv_obj_t *scr = lv_scr_act();
    return scr ? lv_obj_get_height(scr) : 320;
}

static const lv_font_t *get_font_for_screen(void) {
    lv_coord_t h = get_screen_height();
    if (h <= 135) return &lv_font_montserrat_14;
    if (h <= 200) return &lv_font_montserrat_16;
    return &lv_font_montserrat_20;
}

static const lv_font_t *get_small_font_for_screen(void) {
    lv_coord_t h = get_screen_height();
    if (h <= 135) return &lv_font_montserrat_10;
    if (h <= 200) return &lv_font_montserrat_12;
    return &lv_font_montserrat_14;
}

scan_status_t *scan_status_create(const char *message) {
    scan_status_t *ss = calloc(1, sizeof(scan_status_t));
    if (!ss) return NULL;
    
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    lv_color_t bg = lv_color_hex(theme_palette_get_background(theme));
    lv_color_t text = lv_color_hex(theme_palette_get_text(theme));
    lv_color_t accent = lv_color_hex(theme_palette_get_accent(theme));
    
    lv_obj_t *parent = lv_layer_top();
    ss->container = lv_obj_create(parent);
    lv_obj_set_size(ss->container, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(ss->container, 0, 0);
    lv_obj_set_style_bg_color(ss->container, bg, 0);
    lv_obj_set_style_bg_opa(ss->container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ss->container, 0, 0);
    lv_obj_set_style_radius(ss->container, 0, 0);
    lv_obj_set_style_pad_all(ss->container, 0, 0);
    lv_obj_clear_flag(ss->container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ss->container, LV_OBJ_FLAG_CLICKABLE);
    
    display_manager_add_status_bar(ss->container, "Scanning");
    
    lv_coord_t screen_w = get_screen_width();
    lv_coord_t screen_h = get_screen_height();
    
    int spinner_size = (screen_h <= 135) ? 40 : (screen_h <= 200) ? 50 : 60;
    int spinner_y_offset = (screen_h <= 135) ? 50 : (screen_h <= 200) ? 70 : 90;
    
    ss->spinner = lv_spinner_create(ss->container);
    lv_obj_set_size(ss->spinner, spinner_size, spinner_size);
    lv_obj_align(ss->spinner, LV_ALIGN_TOP_MID, 0, spinner_y_offset);
    lv_obj_set_style_arc_width(ss->spinner, spinner_size / 6, LV_PART_MAIN);
    lv_obj_set_style_arc_color(ss->spinner, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_arc_width(ss->spinner, spinner_size / 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(ss->spinner, accent, LV_PART_INDICATOR);
    
    const lv_font_t *font = get_font_for_screen();
    int label_y_offset = spinner_y_offset + spinner_size + 15;
    
    ss->label = lv_label_create(ss->container);
    lv_obj_set_style_text_font(ss->label, font, 0);
    lv_obj_set_style_text_color(ss->label, text, 0);
    lv_obj_set_width(ss->label, screen_w - 40);
    lv_label_set_long_mode(ss->label, LV_LABEL_LONG_WRAP);
    lv_obj_align(ss->label, LV_ALIGN_TOP_MID, 0, label_y_offset);
    lv_label_set_text(ss->label, message ? message : "Scanning");
    
    int progress_y_offset = label_y_offset + 60;
    
    ss->progress_label = lv_label_create(ss->container);
    lv_obj_set_style_text_font(ss->progress_label, get_small_font_for_screen(), 0);
    lv_obj_set_style_text_color(ss->progress_label, text, 0);
    lv_obj_align(ss->progress_label, LV_ALIGN_TOP_MID, 0, progress_y_offset);
    lv_label_set_text(ss->progress_label, "");
    lv_obj_add_flag(ss->progress_label, LV_OBJ_FLAG_HIDDEN);
    
    if (message) {
        strncpy(ss->base_message, message, sizeof(ss->base_message) - 1);
        ss->base_message[sizeof(ss->base_message) - 1] = '\0';
    } else {
        strcpy(ss->base_message, "Scanning");
    }
    
    ss->anim_dots = 0;
    ss->current = 0;
    ss->total = 0;
    ss->active = true;
    
    ss->anim_timer = lv_timer_create(anim_timer_cb, 400, ss);
    
    return ss;
}

void scan_status_update(scan_status_t *ss, const char *message) {
    if (!ss || !ss->active) return;
    
    if (message) {
        strncpy(ss->base_message, message, sizeof(ss->base_message) - 1);
        ss->base_message[sizeof(ss->base_message) - 1] = '\0';
        ss->anim_dots = 0;
    }
}

void scan_status_set_progress(scan_status_t *ss, int current, int total) {
    if (!ss || !ss->active) return;
    
    ss->current = current;
    ss->total = total;
    
    if (total > 0 && ss->progress_label) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d / %d", current, total);
        lv_label_set_text(ss->progress_label, buf);
        lv_obj_clear_flag(ss->progress_label, LV_OBJ_FLAG_HIDDEN);
    }
}

void scan_status_close(scan_status_t *ss) {
    if (!ss) return;
    
    ss->active = false;
    
    if (ss->anim_timer) {
        lv_timer_del(ss->anim_timer);
        ss->anim_timer = NULL;
    }
    
    if (ss->container && lv_obj_is_valid(ss->container)) {
        lv_obj_del(ss->container);
    }
    
    free(ss);
}

bool scan_status_is_active(const scan_status_t *ss) {
    return ss ? ss->active : false;
}
