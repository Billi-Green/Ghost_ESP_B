#ifndef INFRARED_VIEW_H
#define INFRARED_VIEW_H

#include "lvgl/lvgl.h"
#include "managers/display_manager.h"

/**
 * @brief Creates the infrared screen view.
 */
void infrared_view_create(void);

/**
 * @brief Destroys the infrared screen view.
 */
void infrared_view_destroy(void);

/**
 * @brief Handles input events on the infrared view.
 */
void infrared_view_input_cb(InputEvent *event);

extern View infrared_view;

#endif // INFRARED_VIEW_H 