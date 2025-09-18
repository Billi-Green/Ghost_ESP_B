#pragma once

#include "lvgl.h"
#include <stdbool.h>

/*
 * popup.h
 *
 * Lightweight, reusable popup helper for LVGL.
 *
 * Example usage:
 *   popup_t *p = popup_create(parent, 240, 120);
 *   popup_set_title(p, "Warning");
 *   popup_set_body(p, "Are you sure?");
 *   popup_add_button(p, "Cancel", cancel_cb, NULL);
 *   popup_add_button(p, "OK", ok_cb, NULL);
 *   popup_show(p);
 *   popup_destroy(p);
 */

typedef struct popup_t popup_t;

/* create a popup instance. parent may be NULL (screen used). */
popup_t *popup_create(lv_obj_t *parent, int width, int height);

/* set popup title and body text */
void popup_set_title(popup_t *p, const char *title);
void popup_set_body(popup_t *p, const char *body);

/* add a button to the popup; returns the button object or NULL */
lv_obj_t *popup_add_button(popup_t *p, const char *label, lv_event_cb_t event_cb, void *user_data);

/* show/hide/destroy helpers */
void popup_show(popup_t *p);
void popup_hide(popup_t *p);
void popup_destroy(popup_t *p);

/* convenience: create and show a simple popup with buttons */
popup_t *popup_show_simple(lv_obj_t *parent, int width, int height, const char *title, const char *body, const char **buttons, int button_count, lv_event_cb_t *cbs, void **user_datas);

/* lower-level helpers for custom layouts */
lv_obj_t *popup_create_container(lv_obj_t *parent, int width, int height);
lv_obj_t *popup_add_styled_button(lv_obj_t *container, const char *label_text, int btn_w, int btn_h, lv_align_t align, lv_coord_t x_ofs, lv_coord_t y_ofs, const lv_font_t *font, lv_event_cb_t cb, void *user_data);
lv_obj_t *popup_create_title_label(lv_obj_t *container, const char *title, const lv_font_t *font, lv_coord_t y_ofs);
lv_obj_t *popup_create_body_label(lv_obj_t *container, const char *text, lv_coord_t width, bool wrap, const lv_font_t *font, lv_coord_t y_ofs);
