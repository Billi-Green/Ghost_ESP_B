#pragma once

#include <stdbool.h>

#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl/lvgl.h"
#endif

void cst816d_init(void);
bool cst816d_read(lv_indev_drv_t *drv, lv_indev_data_t *data);
