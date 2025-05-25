#ifndef SETTINGS_SCREEN_H
#define SETTINGS_SCREEN_H

#include "lvgl.h"
#include "managers/display_manager.h"
#include "managers/settings_manager.h"

void settings_screen_create(void);
void settings_screen_destroy(void);
void get_settings_screen_callback(void **callback);

extern View settings_screen_view;

#endif 