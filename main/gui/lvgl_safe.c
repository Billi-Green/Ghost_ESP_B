#include "gui/lvgl_safe.h"

void lvgl_obj_del_safe(lv_obj_t **obj)
{
    if (!obj) return;

    if (*obj && lv_obj_is_valid(*obj)) {
        lv_obj_del(*obj);
    }

    *obj = NULL;
}

void lvgl_timer_del_safe(lv_timer_t **timer)
{
    if (!timer) return;

    if (*timer) {
        lv_timer_del(*timer);
    }

    *timer = NULL;
}
