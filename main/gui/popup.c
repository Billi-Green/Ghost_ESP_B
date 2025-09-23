#include "gui/popup.h"
#include "lvgl.h"
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
/*
 * popup.c
 *
 * Implementation of a lightweight popup helper for LVGL.
 */

struct popup_t {
	lv_obj_t *parent;
	lv_obj_t *container;
	lv_obj_t *title_label;
	lv_obj_t *body_label;
	lv_obj_t *btn_container;
	int width;
	int height;
};

static const lv_coord_t DEFAULT_MARGIN = 6;

popup_t *popup_create(lv_obj_t *parent, int width, int height) {
	if (!parent) parent = lv_scr_act();
	popup_t *p = (popup_t*)malloc(sizeof(popup_t));
	if (!p) return NULL;
	memset(p, 0, sizeof(*p));
	p->parent = parent;
	p->width = width;
	p->height = height;
	p->container = lv_obj_create(parent);
	lv_obj_set_size(p->container, width, height);
	lv_obj_align(p->container, LV_ALIGN_TOP_MID, 0, 24);
	lv_obj_set_style_bg_color(p->container, lv_color_hex(0x2E2E2E), 0);
	lv_obj_set_style_border_color(p->container, lv_color_hex(0x555555), 0);
	lv_obj_set_style_border_width(p->container, 2, 0);
	lv_obj_set_style_radius(p->container, 10, 0);
	lv_obj_clear_flag(p->container, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_style_pad_top(p->container, 2, 0);
	lv_obj_set_style_pad_bottom(p->container, 4, 0);
	lv_obj_set_style_pad_left(p->container, DEFAULT_MARGIN, 0);
	lv_obj_set_style_pad_right(p->container, DEFAULT_MARGIN, 0);

	p->title_label = lv_label_create(p->container);
	lv_obj_set_style_text_color(p->title_label, lv_color_hex(0xFFFFFF), 0);
	lv_obj_align(p->title_label, LV_ALIGN_TOP_MID, 0, 10);

	p->body_label = lv_label_create(p->container);
	lv_obj_set_style_text_color(p->body_label, lv_color_hex(0xCCCCCC), 0);
	lv_obj_align(p->body_label, LV_ALIGN_CENTER, 0, -8);

	p->btn_container = lv_obj_create(p->container);
	lv_obj_set_size(p->btn_container, width - (DEFAULT_MARGIN * 2), 40);
	lv_obj_align(p->btn_container, LV_ALIGN_BOTTOM_MID, 0, -8);
	lv_obj_set_style_bg_color(p->btn_container, lv_color_hex(0x000000), 0);
	lv_obj_clear_flag(p->btn_container, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_style_pad_all(p->btn_container, 0, 0);

	lv_obj_add_flag(p->container, LV_OBJ_FLAG_HIDDEN);
	return p;
}

lv_obj_t *popup_create_container(lv_obj_t *parent, int width, int height) {
    if (!parent) parent = lv_scr_act();
    lv_obj_t *container = lv_obj_create(parent);
    lv_obj_set_size(container, width, height);
    lv_obj_align(container, LV_ALIGN_TOP_MID, 0, 24);
    lv_obj_set_style_bg_color(container, lv_color_hex(0x2E2E2E), 0);
    lv_obj_set_style_border_color(container, lv_color_hex(0x555555), 0);
    lv_obj_set_style_border_width(container, 2, 0);
    lv_obj_set_style_radius(container, 10, 0);
    lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_top(container, 2, 0);
    lv_obj_set_style_pad_bottom(container, 4, 0);
    lv_obj_set_style_pad_left(container, DEFAULT_MARGIN, 0);
    lv_obj_set_style_pad_right(container, DEFAULT_MARGIN, 0);
    return container;
}

lv_obj_t *popup_add_styled_button(lv_obj_t *container, const char *label_text, int btn_w, int btn_h, lv_align_t align, lv_coord_t x_ofs, lv_coord_t y_ofs, const lv_font_t *font, lv_event_cb_t cb, void *user_data) {
    if (!container) return NULL;
    lv_obj_t *btn = lv_btn_create(container);
    lv_obj_set_size(btn, btn_w, btn_h);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x444444), 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(0x666666), 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_align(btn, align, x_ofs, y_ofs);
    if (cb) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, label_text ? label_text : "");
    if (font) lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_center(lbl);
    return btn;
}

lv_obj_t *popup_create_title_label(lv_obj_t *container, const char *title, const lv_font_t *font, lv_coord_t y_ofs) {
    if (!container) return NULL;
    lv_obj_t *lbl = lv_label_create(container);
    if (font) lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(lbl, title ? title : "");
    lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, y_ofs);
    return lbl;
}

lv_obj_t *popup_create_body_label(lv_obj_t *container, const char *text, lv_coord_t width, bool wrap, const lv_font_t *font, lv_coord_t y_ofs) {
    if (!container) return NULL;
    lv_obj_t *lbl = lv_label_create(container);
    if (width > 0) lv_obj_set_width(lbl, width);
    if (wrap) lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    if (font) lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xDDDDDD), 0);
    lv_label_set_text(lbl, text ? text : "");
    lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, y_ofs);
    return lbl;
}

void popup_set_title(popup_t *p, const char *title) {
	if (!p || !p->title_label) return;
	lv_label_set_text(p->title_label, title ? title : "");
}

void popup_set_body(popup_t *p, const char *body) {
	if (!p || !p->body_label) return;
	lv_label_set_text(p->body_label, body ? body : "");
}

lv_obj_t *popup_add_button(popup_t *p, const char *label, lv_event_cb_t event_cb, void *user_data) {
	if (!p || !p->btn_container) return NULL;
	lv_obj_t *btn = lv_btn_create(p->btn_container);
	int btn_w = (p->width - (DEFAULT_MARGIN * 2) - 8) / 2; // default width for up to 2 buttons
	lv_obj_set_size(btn, btn_w, 32);
	lv_obj_set_style_bg_color(btn, lv_color_hex(0x444444), 0);
	lv_obj_set_style_border_color(btn, lv_color_hex(0x666666), 0);
	lv_obj_set_style_border_width(btn, 1, 0);
	lv_obj_add_event_cb(btn, event_cb, LV_EVENT_CLICKED, user_data);

	lv_obj_t *lbl = lv_label_create(btn);
	lv_label_set_text(lbl, label ? label : "");
	lv_obj_center(lbl);

	// position buttons horizontally
	static int btn_offset = 0;
	lv_obj_align(btn, LV_ALIGN_LEFT_MID, 8 + btn_offset, 0);
	btn_offset += btn_w + 8;

	return btn;
}

void popup_show(popup_t *p) {
	if (!p || !p->container) return;
	lv_obj_clear_flag(p->container, LV_OBJ_FLAG_HIDDEN);
}

void popup_hide(popup_t *p) {
	if (!p || !p->container) return;
	lv_obj_add_flag(p->container, LV_OBJ_FLAG_HIDDEN);
}

void popup_destroy(popup_t *p) {
	if (!p) return;
	if (p->container && lv_obj_is_valid(p->container)) lv_obj_del(p->container);
	free(p);
}

popup_t *popup_show_simple(lv_obj_t *parent, int width, int height, const char *title, const char *body, const char **buttons, int button_count, lv_event_cb_t *cbs, void **user_datas) {
	popup_t *p = popup_create(parent, width, height);
	if (!p) return NULL;
	popup_set_title(p, title);
	popup_set_body(p, body);
	for (int i = 0; i < button_count; ++i) {
		lv_event_cb_t cb = (cbs && cbs[i]) ? cbs[i] : NULL;
		void *ud = (user_datas && user_datas[i]) ? user_datas[i] : NULL;
		popup_add_button(p, buttons[i], cb, ud);
	}
	popup_show(p);
	return p;
}

void popup_set_button_selected(lv_obj_t *btn, bool selected) {
    if (!btn || !lv_obj_is_valid(btn)) return;
    if (selected) {
        lv_obj_set_style_bg_color(btn, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(btn, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_t *lbl = lv_obj_get_child(btn, 0);
        if (lbl) lv_obj_set_style_text_color(lbl, lv_color_hex(0x000000), 0);
    } else {
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x444444), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(btn, lv_color_hex(0x666666), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_t *lbl = lv_obj_get_child(btn, 0);
        if (lbl) lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
    }
}

void popup_update_selection(lv_obj_t **btns, int count, int selected_index) {
    if (!btns || count <= 0) return;
    for (int i = 0; i < count; ++i) {
        lv_obj_t *b = btns[i];
        if (!b || !lv_obj_is_valid(b)) continue;
        popup_set_button_selected(b, i == selected_index);
    }
}

lv_obj_t *popup_create_scroll_area(
    lv_obj_t *parent,
    lv_coord_t w,
    lv_coord_t h,
    lv_align_t align,
    lv_coord_t x_ofs,
    lv_coord_t y_ofs
) {
    if (!parent) parent = lv_scr_act();
    lv_obj_t *scroll = lv_obj_create(parent);
    lv_obj_set_size(scroll, w, h);
    lv_obj_align(scroll, align, x_ofs, y_ofs);
    // Transparent background and no border to blend with popup
    lv_obj_set_style_bg_opa(scroll, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(scroll, 0, 0);
    // Scroll behavior
    lv_obj_set_scroll_dir(scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(scroll, LV_SCROLLBAR_MODE_AUTO);
    // Minimal padding so text aligns nicely to top-left
    lv_obj_set_style_pad_all(scroll, 0, 0);
    return scroll;
}

void popup_layout_buttons_row(
    lv_obj_t *container,
    lv_obj_t **btns,
    int count,
    lv_coord_t btn_w,
    lv_coord_t btn_h,
    lv_coord_t y,
    lv_coord_t gap
) {
    if (!container || !btns || count <= 0) return;
    lv_coord_t cw = lv_obj_get_width(container);
    lv_coord_t total_w = count * btn_w + (count - 1) * gap;
    lv_coord_t start_x = (cw > total_w) ? (cw - total_w) / 2 : 0;
    for (int i = 0; i < count; ++i) {
        lv_obj_t *b = btns[i];
        if (!b || !lv_obj_is_valid(b)) continue;
        lv_obj_set_size(b, btn_w, btn_h);
        lv_obj_align(b, LV_ALIGN_TOP_LEFT, start_x + i * (btn_w + gap), y);
        // Ensure label (first child) is centered within button
        lv_obj_t *lbl = lv_obj_get_child(b, 0);
        if (lbl) lv_obj_center(lbl);
    }
}
