#include "gui/detail_view.h"
#include "managers/display_manager.h"
#include "managers/settings_manager.h"
#include "gui/theme_palette_api.h"
#include "lvgl.h"
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

uint32_t theme_palette_get_background(uint8_t theme);
uint32_t theme_palette_get_surface(uint8_t theme);
uint32_t theme_palette_get_surface_alt(uint8_t theme);
uint32_t theme_palette_get_text(uint8_t theme);
uint32_t theme_palette_get_accent(uint8_t theme);
bool theme_palette_is_bright(uint8_t theme);

typedef struct {
    lv_obj_t *obj;
    detail_row_type_t type;
    bool selectable;
} detail_row_t;

struct detail_view_t {
    lv_obj_t *list;
    lv_style_t style_item;
    lv_style_t style_item_alt;
    lv_style_t style_selected;
    lv_style_t style_header;
    lv_style_t style_divider;
    detail_row_t *rows;
    int count;
    int capacity;
    int selected;
    int btn_h;
    int first_selectable;
};

static inline void ensure_capacity(detail_view_t *dv, int need) {
    if (dv->capacity >= need) return;
    int newcap = dv->capacity ? dv->capacity * 2 : 16;
    if (newcap < need) newcap = need;
    detail_row_t *new_rows = (detail_row_t *)realloc(dv->rows, sizeof(detail_row_t) * newcap);
    if (!new_rows) return;
    dv->rows = new_rows;
    dv->capacity = newcap;
}

static inline lv_style_t *get_zebra_style(detail_view_t *dv, int idx) {
    bool zebra = settings_get_zebra_menus_enabled(&G_Settings);
    if (!zebra) return &dv->style_item;
    return (idx % 2 == 0) ? &dv->style_item : &dv->style_item_alt;
}

static inline const lv_font_t *get_item_font(const detail_view_t *dv) {
    return (dv->btn_h <= 40) ? &lv_font_montserrat_12 : &lv_font_montserrat_14;
}

static inline const lv_font_t *get_value_font(const detail_view_t *dv) {
    return (dv->btn_h <= 40) ? &lv_font_montserrat_12 : &lv_font_montserrat_14;
}

static inline void get_theme_colors(lv_color_t *bg, lv_color_t *surface, lv_color_t *surface_alt, lv_color_t *text, lv_color_t *accent) {
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    if (bg) *bg = lv_color_hex(theme_palette_get_background(theme));
    if (surface) *surface = lv_color_hex(theme_palette_get_surface(theme));
    if (surface_alt) *surface_alt = lv_color_hex(theme_palette_get_surface_alt(theme));
    if (text) *text = lv_color_hex(theme_palette_get_text(theme));
    if (accent) *accent = lv_color_hex(theme_palette_get_accent(theme));
}

static void apply_selected_style(detail_view_t *dv, lv_obj_t *item, bool on) {
    if (!item || !lv_obj_is_valid(item)) return;
    
    lv_color_t text, accent;
    get_theme_colors(NULL, NULL, NULL, &text, &accent);
    
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    lv_color_t sel_text = theme_palette_is_bright(theme) ? lv_color_hex(0x000000) : lv_color_hex(0xFFFFFF);
    
    if (on) {
        lv_style_set_bg_color(&dv->style_selected, accent);
        lv_style_set_bg_grad_color(&dv->style_selected, accent);
        lv_obj_add_style(item, &dv->style_selected, 0);
        
        uint32_t child_cnt = lv_obj_get_child_cnt(item);
        for (uint32_t i = 0; i < child_cnt; ++i) {
            lv_obj_t *child = lv_obj_get_child(item, (int32_t)i);
            if (!child) continue;
            void *ud = lv_obj_get_user_data(child);
            if (ud == (void *)1 || ud == (void *)2) {
                lv_obj_set_style_text_color(child, sel_text, 0);
            }
            if (ud == (void *)2) {
                lv_obj_clear_flag(child, LV_OBJ_FLAG_HIDDEN);
            }
        }
    } else {
        lv_obj_remove_style(item, &dv->style_selected, 0);
        
        uint32_t child_cnt = lv_obj_get_child_cnt(item);
        for (uint32_t i = 0; i < child_cnt; ++i) {
            lv_obj_t *child = lv_obj_get_child(item, (int32_t)i);
            if (!child) continue;
            void *ud = lv_obj_get_user_data(child);
            if (ud == (void *)1 || ud == (void *)2) {
                lv_obj_set_style_text_color(child, text, 0);
            }
            if (ud == (void *)2) {
#ifndef CONFIG_USE_TOUCHSCREEN
                lv_obj_add_flag(child, LV_OBJ_FLAG_HIDDEN);
#endif
            }
        }
    }
}

static int find_next_selectable(detail_view_t *dv, int start, int dir) {
    int idx = start;
    for (int i = 0; i < dv->count; i++) {
        idx += dir;
        if (idx < 0) idx = dv->count - 1;
        if (idx >= dv->count) idx = 0;
        if (dv->rows[idx].selectable) return idx;
    }
    return start;
}

detail_view_t *detail_view_create(lv_obj_t *parent, const char *title) {
    if (!parent) parent = lv_scr_act();
    detail_view_t *dv = (detail_view_t *)calloc(1, sizeof(detail_view_t));
    if (!dv) return NULL;
    
    int w = LV_HOR_RES;
    int h = LV_VER_RES;
    int STATUS_BAR_HEIGHT = 20;
    bool small = (w <= 240 || h <= 240);
    dv->btn_h = small ? 36 : 44;
    dv->selected = -1;
    dv->first_selectable = -1;
    
    lv_color_t bg, surface, surface_alt, text;
    get_theme_colors(&bg, &surface, &surface_alt, &text, NULL);
    
    dv->list = lv_list_create(parent);
    lv_obj_set_size(dv->list, w, h - STATUS_BAR_HEIGHT);
    lv_obj_align(dv->list, LV_ALIGN_TOP_MID, 0, STATUS_BAR_HEIGHT);
    lv_obj_set_style_bg_color(dv->list, bg, 0);
    lv_obj_set_style_pad_all(dv->list, 0, 0);
    lv_obj_set_style_border_width(dv->list, 0, 0);
    lv_obj_set_style_radius(dv->list, 0, 0);
    
    lv_style_init(&dv->style_item);
    lv_style_set_bg_color(&dv->style_item, surface);
    lv_style_set_bg_opa(&dv->style_item, LV_OPA_COVER);
    lv_style_set_border_width(&dv->style_item, 0);
    lv_style_set_radius(&dv->style_item, 0);
    
    lv_style_init(&dv->style_item_alt);
    lv_style_set_bg_color(&dv->style_item_alt, surface_alt);
    lv_style_set_bg_opa(&dv->style_item_alt, LV_OPA_COVER);
    lv_style_set_border_width(&dv->style_item_alt, 0);
    lv_style_set_radius(&dv->style_item_alt, 0);
    
    lv_style_init(&dv->style_selected);
    lv_style_set_bg_opa(&dv->style_selected, LV_OPA_COVER);
    lv_style_set_radius(&dv->style_selected, 0);
    lv_style_set_bg_grad_dir(&dv->style_selected, LV_GRAD_DIR_NONE);
    
    lv_style_init(&dv->style_header);
    lv_style_set_bg_color(&dv->style_header, surface);
    lv_style_set_bg_opa(&dv->style_header, LV_OPA_30);
    lv_style_set_border_width(&dv->style_header, 0);
    lv_style_set_radius(&dv->style_header, 0);
    
    lv_style_init(&dv->style_divider);
    lv_style_set_bg_color(&dv->style_divider, text);
    lv_style_set_bg_opa(&dv->style_divider, LV_OPA_20);
    lv_style_set_border_width(&dv->style_divider, 0);
    lv_style_set_radius(&dv->style_divider, 0);
    
    if (title && *title) display_manager_add_status_bar(title);
    
    return dv;
}

void detail_view_destroy(detail_view_t *dv) {
    if (!dv) return;
    if (dv->list && lv_obj_is_valid(dv->list)) lv_obj_del(dv->list);
    free(dv->rows);
    free(dv);
}

void detail_view_add_info(detail_view_t *dv, const char *label, const char *value) {
    if (!dv || !dv->list) return;
    ensure_capacity(dv, dv->count + 1);
    
    int zebra_idx = 0;
    for (int i = 0; i < dv->count; i++) {
        if (dv->rows[i].type == DETAIL_ROW_ACTION) zebra_idx++;
    }
    
    lv_obj_t *btn = lv_list_add_btn(dv->list, NULL, NULL);
    if (!btn) return;
    
    lv_obj_set_height(btn, dv->btn_h);
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(btn, 8, 0);
    lv_obj_set_style_pad_right(btn, 8, 0);
    lv_obj_add_style(btn, get_zebra_style(dv, zebra_idx), 0);
    
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, label ? label : "");
    lv_obj_set_style_text_font(lbl, get_item_font(dv), 0);
    lv_color_t text;
    get_theme_colors(NULL, NULL, NULL, &text, NULL);
    lv_obj_set_style_text_color(lbl, text, 0);
    lv_obj_set_user_data(lbl, (void *)1);
    
    lv_obj_t *val = lv_label_create(btn);
    lv_label_set_text(val, value ? value : "");
    lv_obj_set_style_text_font(val, get_value_font(dv), 0);
    lv_obj_set_style_text_color(val, text, 0);
    lv_obj_set_user_data(val, (void *)1);
    
    dv->rows[dv->count].obj = btn;
    dv->rows[dv->count].type = DETAIL_ROW_INFO;
    dv->rows[dv->count].selectable = false;
    dv->count++;
}

void detail_view_add_infof(detail_view_t *dv, const char *label, const char *fmt, ...) {
    char buf[128];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    detail_view_add_info(dv, label, buf);
}

void detail_view_add_action(detail_view_t *dv, const char *label, lv_event_cb_t on_click, void *user_data) {
    if (!dv || !dv->list) return;
    ensure_capacity(dv, dv->count + 1);
    
    int zebra_idx = 0;
    for (int i = 0; i < dv->count; i++) {
        if (dv->rows[i].type == DETAIL_ROW_ACTION) zebra_idx++;
    }
    
    lv_obj_t *btn = lv_list_add_btn(dv->list, NULL, label ? label : "");
    if (!btn) return;
    
    lv_obj_set_height(btn, dv->btn_h);
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(btn, 8, 0);
    lv_obj_set_style_pad_right(btn, 8, 0);
    lv_obj_add_style(btn, get_zebra_style(dv, zebra_idx), 0);
    
    if (on_click) {
        lv_obj_add_event_cb(btn, on_click, LV_EVENT_CLICKED, user_data);
    }
    
    lv_obj_t *lbl = lv_obj_get_child(btn, 0);
    if (lbl) {
        lv_obj_set_style_text_font(lbl, get_item_font(dv), 0);
        lv_color_t text;
        get_theme_colors(NULL, NULL, NULL, &text, NULL);
        lv_obj_set_style_text_color(lbl, text, 0);
        lv_obj_set_width(lbl, LV_PCT(100));
        lv_obj_set_user_data(lbl, (void *)1);
    }
    
    lv_obj_t *arrow = lv_label_create(btn);
    lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_font(arrow, get_item_font(dv), 0);
    lv_color_t text;
    get_theme_colors(NULL, NULL, NULL, &text, NULL);
    lv_obj_set_style_text_color(arrow, text, 0);
    lv_obj_set_user_data(arrow, (void *)2);
#ifndef CONFIG_USE_TOUCHSCREEN
    lv_obj_add_flag(arrow, LV_OBJ_FLAG_HIDDEN);
#endif
    
    dv->rows[dv->count].obj = btn;
    dv->rows[dv->count].type = DETAIL_ROW_ACTION;
    dv->rows[dv->count].selectable = true;
    
    if (dv->first_selectable < 0) {
        dv->first_selectable = dv->count;
    }
    
    if (dv->selected < 0 && dv->first_selectable == dv->count) {
        dv->selected = dv->count;
        apply_selected_style(dv, btn, true);
    }
    
    dv->count++;
}

void detail_view_add_header(detail_view_t *dv, const char *text) {
    if (!dv || !dv->list) return;
    ensure_capacity(dv, dv->count + 1);
    
    lv_obj_t *btn = lv_list_add_btn(dv->list, NULL, text ? text : "");
    if (!btn) return;
    
    lv_obj_set_height(btn, dv->btn_h);
    lv_obj_add_style(btn, &dv->style_header, 0);
    
    lv_obj_t *lbl = lv_obj_get_child(btn, 0);
    if (lbl) {
        const lv_font_t *font = (dv->btn_h <= 40) ? &lv_font_montserrat_12 : &lv_font_montserrat_14;
        lv_obj_set_style_text_font(lbl, font, 0);
        lv_color_t txt;
        get_theme_colors(NULL, NULL, NULL, &txt, NULL);
        lv_obj_set_style_text_color(lbl, txt, 0);
    }
    
    dv->rows[dv->count].obj = btn;
    dv->rows[dv->count].type = DETAIL_ROW_HEADER;
    dv->rows[dv->count].selectable = false;
    dv->count++;
}

void detail_view_add_divider(detail_view_t *dv) {
    if (!dv || !dv->list) return;
    ensure_capacity(dv, dv->count + 1);
    
    lv_obj_t *line = lv_obj_create(dv->list);
    lv_obj_set_size(line, LV_PCT(100), 2);
    lv_obj_add_style(line, &dv->style_divider, 0);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE);
    
    dv->rows[dv->count].obj = line;
    dv->rows[dv->count].type = DETAIL_ROW_DIVIDER;
    dv->rows[dv->count].selectable = false;
    dv->count++;
}

lv_obj_t *detail_view_add_back(detail_view_t *dv, lv_event_cb_t on_click, void *user_data) {
    detail_view_add_action(dv, LV_SYMBOL_LEFT " Back", on_click, user_data);
    return dv->rows[dv->count - 1].obj;
}

void detail_view_set_selected(detail_view_t *dv, int index) {
    if (!dv || dv->count == 0) return;
    
    index = find_next_selectable(dv, index - 1, 1);
    
    if (index < 0 || index >= dv->count) {
        index = dv->first_selectable >= 0 ? dv->first_selectable : 0;
    }
    if (!dv->rows[index].selectable) {
        index = find_next_selectable(dv, index, 1);
    }
    if (dv->selected == index) return;
    
    if (dv->selected >= 0 && dv->selected < dv->count) {
        apply_selected_style(dv, dv->rows[dv->selected].obj, false);
    }
    
    dv->selected = index;
    apply_selected_style(dv, dv->rows[dv->selected].obj, true);
    lv_obj_scroll_to_view(dv->rows[dv->selected].obj, LV_ANIM_OFF);
}

void detail_view_move_selection(detail_view_t *dv, int delta) {
    if (!dv || dv->count == 0 || dv->first_selectable < 0) return;
    
    int new_idx = find_next_selectable(dv, dv->selected, delta > 0 ? 1 : -1);
    detail_view_set_selected(dv, new_idx);
}

int detail_view_get_selected(const detail_view_t *dv) {
    return dv ? dv->selected : -1;
}

int detail_view_get_count(const detail_view_t *dv) {
    return dv ? dv->count : 0;
}

detail_row_type_t detail_view_get_row_type(const detail_view_t *dv, int index) {
    if (!dv || index < 0 || index >= dv->count) return DETAIL_ROW_INFO;
    return dv->rows[index].type;
}

void detail_view_clear(detail_view_t *dv) {
    if (!dv || !dv->list) return;
    lv_obj_clean(dv->list);
    for (int i = 0; i < dv->count; ++i) {
        dv->rows[i].obj = NULL;
    }
    dv->count = 0;
    dv->selected = -1;
    dv->first_selectable = -1;
}

lv_obj_t *detail_view_get_list(detail_view_t *dv) {
    return dv ? dv->list : NULL;
}

void detail_view_refresh_styles(detail_view_t *dv) {
    if (!dv) return;
    
    lv_color_t bg, surface, surface_alt, text;
    get_theme_colors(&bg, &surface, &surface_alt, &text, NULL);
    
    lv_style_set_bg_color(&dv->style_item, surface);
    lv_style_set_bg_color(&dv->style_item_alt, surface_alt);
    lv_style_set_bg_color(&dv->style_header, surface);
    lv_style_set_bg_opa(&dv->style_header, LV_OPA_30);
    lv_style_set_bg_color(&dv->style_divider, text);
    lv_style_set_bg_opa(&dv->style_divider, LV_OPA_20);
    
    for (int i = 0; i < dv->count; i++) {
        lv_obj_t *obj = dv->rows[i].obj;
        if (!obj || !lv_obj_is_valid(obj)) continue;
        
        if (dv->rows[i].type == DETAIL_ROW_INFO || dv->rows[i].type == DETAIL_ROW_ACTION) {
            lv_obj_remove_style(obj, &dv->style_item, 0);
            lv_obj_remove_style(obj, &dv->style_item_alt, 0);
            lv_obj_add_style(obj, get_zebra_style(dv, i), 0);
            
            uint32_t child_cnt = lv_obj_get_child_cnt(obj);
            for (uint32_t c = 0; c < child_cnt; c++) {
                lv_obj_t *child = lv_obj_get_child(obj, (int32_t)c);
                if (child && lv_obj_get_user_data(child)) {
                    lv_obj_set_style_text_color(child, text, 0);
                }
            }
        }
    }
    
    if (dv->selected >= 0 && dv->selected < dv->count) {
        apply_selected_style(dv, dv->rows[dv->selected].obj, true);
    }
}
