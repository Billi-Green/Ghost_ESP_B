#pragma once

#include "lvgl.h"

// simple reusable popup wrapper
// usage: popup_t *p = popup_create(parent, width, height);
// popup_set_title(p, "Title");
// popup_set_body(p, "Body text");
// popup_add_button(p, "Cancel", cb, user_data);
// popup_add_button(p, "OK", cb2, user_data2);
// popup_show(p);
// popup_destroy(p);

typedef struct popup_t popup_t;

popup_t *popup_create(lv_obj_t *parent, int width, int height);
void popup_set_title(popup_t *p, const char *title);
void popup_set_body(popup_t *p, const char *body);
lv_obj_t *popup_add_button(popup_t *p, const char *label, lv_event_cb_t event_cb, void *user_data);
void popup_show(popup_t *p);
void popup_hide(popup_t *p);
void popup_destroy(popup_t *p);

// convenience: create, set text, add buttons, and show
popup_t *popup_show_simple(lv_obj_t *parent, int width, int height, const char *title, const char *body, const char **buttons, int button_count, lv_event_cb_t *cbs, void **user_datas);

// create a styled container suitable for popups (returns an lv_obj_t* container)
lv_obj_t *popup_create_container(lv_obj_t *parent, int width, int height);
