#ifndef CLOCK_SCREEN_H
#define CLOCK_SCREEN_H

#include "lvgl.h"
#include "managers/display_manager.h"

LV_IMG_DECLARE(watch_bg);
LV_IMG_DECLARE(hour);
LV_IMG_DECLARE(minute);
LV_IMG_DECLARE(second);

void clock_create(void);
void clock_destroy(void);
void get_clock_callback(void **callback);

extern View clock_view;
extern lv_timer_t *clock_timer;

#endif /* CLOCK_SCREEN_H */ 