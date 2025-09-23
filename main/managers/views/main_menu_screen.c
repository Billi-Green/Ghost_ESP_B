#include "managers/views/main_menu_screen.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "lvgl.h"
#include "managers/views/app_gallery_screen.h"
#include <stdio.h>
#include <stdlib.h>
#include "managers/views/clock_screen.h"
#include "managers/views/settings_screen.h"
#ifdef CONFIG_HAS_NFC
#include "managers/views/nfc_view.h"
#endif
#if CONFIG_HAS_INFRARED
#include "managers/views/infrared_view.h"
#endif

static const char *TAG = "MainMenu";

#define ANIM_DURATION 60 // Animation duration in milliseconds HIGH: 30, LOW: 120

// Menu layout types
typedef enum {
    MENU_LAYOUT_CAROUSEL = 0, // Current single-item carousel
    MENU_LAYOUT_GRID = 1,     // Grid layout (unused by setting)
    MENU_LAYOUT_GRID_CARDS = 2 // Grid-style card layout
} MenuLayoutType;

lv_obj_t *menu_container;
static int selected_item_index = 0;
static int touch_start_x;
static int touch_start_y;
static bool touch_started = false;
static bool is_animating = false;
static const int SWIPE_THRESHOLD = 50;
static const int TAP_THRESHOLD = 10; // Add a threshold for tap detection
static MenuLayoutType current_layout = MENU_LAYOUT_CAROUSEL;

// Grid layout variables
static lv_obj_t **grid_buttons = NULL;
static int grid_rows = 0;
static int grid_cols = 0;

// Grid layout variables
static lv_obj_t *grid_cards_container = NULL;
static lv_obj_t **grid_cards = NULL;
static int grid_card_width = 0;
static int grid_card_height = 0;

const View *pending_view_to_switch = NULL;
static EOptionsMenuType pending_menu_type;
static bool menu_item_selected = false;

typedef struct {
  const char *name;
  const lv_img_dsc_t *icon;
  const int palette_index; // pick a color 0-5 to assign the menu item
  lv_color_t border_color;
} menu_item_t;

// Define colors as compile-time constants
menu_item_t menu_items[] = {
#ifndef CONFIG_IDF_TARGET_ESP32S2
    {"BLE", &bluetooth, 0},
#endif
    {"WiFi", &wifi, 1}, // applies to all boards
#ifdef CONFIG_HAS_GPS
    {"GPS", &Map, 2},
#endif
#if CONFIG_HAS_INFRARED
    {"Infrared", &infrared, 0}, // main infrared icon
#endif
#ifdef CONFIG_HAS_NFC
    {"NFC", &nfc_icon, 2},
#endif
    {"Apps", &GESPAppGallery, 3}, // applies to all boards
#ifdef CONFIG_HAS_RTC_CLOCK
    {"Clock", &clock_icon, 4},
#endif
    {"Settings", &settings_icon, 5} // applies to all boards
};

static int num_items = sizeof(menu_items) / sizeof(menu_items[0]);
lv_obj_t *current_item_obj = NULL;

// Add navigation button objects at file scope
static lv_obj_t *left_nav_btn = NULL;
static lv_obj_t *right_nav_btn = NULL;

static void init_menu_colors(void) {
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    const uint32_t palettes[15][6] = { // if more menu items are added this will need to expand, or reuse colors
// bluetooth colors,wifi colors,GPS colors,Apps colors,Clock Colors,Settings colors
        {0x1976D2,0xD32F2F,0x388E3C,0x7B1FA2,0x000000,0xFF9800}, // default
        {0xFFCDD2,0xC8E6C9,0xB3E5FC,0xFFF9C4,0xD1C4E9,0xCFD8DC}, // Pastel
        {0x263238,0x37474F,0x455A64,0x546E7A,0x263238,0x37474F}, // Dark
        {0xFFFFFF,0xFFFFFF,0xFFFFFF,0xFFFFFF,0xFFFFFF,0xFFFFFF}, // Bright
        {0x002B36,0x073642,0x586E75,0x839496,0xEEE8D5,0x002B36}, // Solarized
        {0x888888,0x888888,0x888888,0x888888,0x888888,0x888888}, // Monochrome
        {0xE91E63,0xE91E63,0xE91E63,0xE91E63,0xE91E63,0xE91E63}, // Rose Red
        {0x9C27B0,0x9C27B0,0x9C27B0,0x9C27B0,0x9C27B0,0x9C27B0}, // Purple
        {0x2196F3,0x2196F3,0x2196F3,0x2196F3,0x2196F3,0x2196F3}, // Blue
        {0xFFA500,0xFFA500,0xFFA500,0xFFA500,0xFFA500,0xFFA500}, // Orange
        {0x39FF14,0xFF073A,0x0FF1CE,0xF8F32B,0xFF6EC7,0xFF8C00}, // Neon
        {0xFF00FF,0x00FFFF,0xFF0000,0x00FF00,0xFFFF00,0x800080}, // Cyberpunk
        {0x0077BE,0x00CED1,0x20B2AA,0x4682B4,0x5F9EA0,0x00008B}, // Ocean
        {0xFF4500,0xFF8C00,0xFFD700,0xFF1493,0x8B008B,0x2E0854}, // Sunset
        {0x556B2F,0x6B8E23,0x228B22,0x2E8B57,0x8FBC8F,0x8B4513}  // Forest
    };
    for (int i = 0; i < num_items; i++) { 
        // bug here - we used to assume that the index of each menu item never changes. By removing options we dont need their index can change
        // perhaps this could be fixed by adding an enabled bool for each menu item, and only drawing the menu icon if enabled
        menu_items[i].border_color = lv_color_hex(palettes[theme][menu_items[i].palette_index]);
    }
}

// Animation callback wrapper
static void anim_set_x(void *obj, int32_t v) {
    lv_obj_set_x((lv_obj_t *)obj, (lv_coord_t)v);
}

// Add this helper at file scope if not present:
static void fade_out_ready_cb(lv_anim_t *a) {
    lv_obj_del((lv_obj_t *)a->var);
}

static void fade_in_ready_cb(lv_anim_t *a) {
    is_animating = false;
}

static void anim_set_opa(void *obj, int32_t v) {
    lv_obj_set_style_opa((lv_obj_t *)obj, v, 0);
}

static void anim_set_scale(void *obj, int32_t v) {
    lv_obj_set_style_transform_zoom((lv_obj_t *)obj, v, 0);
}

static void anim_set_bg_color(void *obj, int32_t v) {
    // v is a 24-bit RGB value
    lv_color_t color = lv_color_hex(v);
    lv_obj_set_style_bg_color((lv_obj_t *)obj, color, LV_PART_MAIN);
}

// Timer callback to restore button color
// Restore label color timer callback (used since buttons are transparent)
static void restore_label_color_cb(lv_timer_t *timer) {
    lv_obj_t *label_obj = (lv_obj_t *)timer->user_data;
    // restore label text color to white
    lv_obj_set_style_text_color(label_obj, lv_color_hex(0xFFFFFF), 0);
    lv_timer_del(timer);
}

// Helper function to animate navigation button press by highlighting the arrow label
static void animate_nav_button_press(lv_obj_t *btn) {
    // Find first child (the label containing the arrow) - child id 0
    lv_obj_t *label = lv_obj_get_child(btn, 0);
    if (!label) return;

    // Set a temporary highlight color for the label
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFF00), 0);

    // Return label to original color after a short delay
    lv_timer_create(restore_label_color_cb, 150, label);
}

static void button_click_anim_cb(lv_anim_t *a) {
    if (pending_view_to_switch) {
        if (pending_view_to_switch == &options_menu_view)
            SelectedMenuType = pending_menu_type;
        display_manager_switch_view((View *)pending_view_to_switch);
        pending_view_to_switch = NULL;
    }
}
static void animate_button_click(lv_obj_t *btn) {
    // Animate opacity down and back up
    int anim_duration = ANIM_DURATION / 8; // Half duration for click effect - divide by 8 for faster click effect
    if (anim_duration < 10) anim_duration = 10; // Ensure minimum duration
    
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, btn);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_50);
    lv_anim_set_time(&a, anim_duration);
    lv_anim_set_exec_cb(&a, anim_set_opa);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);

    lv_anim_t a2;
    lv_anim_init(&a2);
    lv_anim_set_var(&a2, btn);
    lv_anim_set_values(&a2, LV_OPA_50, LV_OPA_COVER);
    lv_anim_set_time(&a2, anim_duration);
    lv_anim_set_exec_cb(&a2, anim_set_opa);
    lv_anim_set_path_cb(&a2, lv_anim_path_ease_in);
    lv_anim_set_ready_cb(&a2, button_click_anim_cb);
    lv_anim_start(&a2);
    
}

static void update_menu_item(bool slide_left) {
    static lv_obj_t *prev_item_obj = NULL;
    is_animating = true; // Set flag to block input during animation
    // Animate out old item if it exists
    if (current_item_obj) {
        prev_item_obj = current_item_obj;
        // Slide out
        lv_anim_t anim_out;
        lv_anim_init(&anim_out);
        lv_anim_set_var(&anim_out, prev_item_obj);
        int end_x = slide_left ? -LV_HOR_RES : LV_HOR_RES;
        lv_anim_set_values(&anim_out, 0, end_x);
        lv_anim_set_time(&anim_out, ANIM_DURATION);
        lv_anim_set_path_cb(&anim_out, lv_anim_path_ease_in_out);
        lv_anim_set_exec_cb(&anim_out, anim_set_x);
        if (!menu_item_selected) { // Only delete if not selecting
            lv_anim_set_ready_cb(&anim_out, fade_out_ready_cb);
        }
        lv_anim_start(&anim_out);

        // Fade out
        lv_anim_t fade_out;
        lv_anim_init(&fade_out);
        lv_anim_set_var(&fade_out, prev_item_obj);
        lv_anim_set_values(&fade_out, LV_OPA_COVER, LV_OPA_TRANSP);
        lv_anim_set_time(&fade_out, ANIM_DURATION);
        lv_anim_set_exec_cb(&fade_out, anim_set_opa);
        lv_anim_start(&fade_out);
    }

    // Create new item (off-screen, transparent)
    current_item_obj = lv_btn_create(menu_container);
    lv_obj_set_style_bg_color(current_item_obj, lv_color_hex(0x1E1E1E), LV_PART_MAIN);
    lv_obj_set_style_shadow_width(current_item_obj, 3, LV_PART_MAIN);
    lv_obj_set_style_shadow_color(current_item_obj, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_border_width(current_item_obj, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(current_item_obj, menu_items[selected_item_index].border_color, LV_PART_MAIN);
    lv_obj_set_style_radius(current_item_obj, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_all(current_item_obj, 0, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(current_item_obj, false, 0);

    int btn_size = LV_MIN(LV_HOR_RES, LV_VER_RES) * 0.6;
    if (LV_HOR_RES <= 128 && LV_VER_RES <= 128) {
        btn_size = 80;
    }
    lv_obj_set_size(current_item_obj, btn_size, btn_size);

    // Start new item off-screen (opposite direction of swipe)
    int start_x = slide_left ? LV_HOR_RES : -LV_HOR_RES;
    lv_obj_align(current_item_obj, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_opa(current_item_obj, LV_OPA_TRANSP, 0); // Start transparent

    lv_obj_t *icon = lv_img_create(current_item_obj);
    lv_img_set_src(icon, menu_items[selected_item_index].icon);

    const int icon_size = 50;
    lv_obj_set_size(icon, icon_size, icon_size);
    lv_img_set_size_mode(icon, LV_IMG_SIZE_MODE_REAL);
    lv_img_set_antialias(icon, false);
    if (strcmp(menu_items[selected_item_index].name,"Clock")) {
        lv_obj_set_style_img_recolor(icon, menu_items[selected_item_index].border_color, 0);
        lv_obj_set_style_img_recolor_opa(icon, LV_OPA_COVER, 0);
    }
    lv_obj_set_style_clip_corner(icon, false, 0);

    int icon_x_offset = -3;
    int icon_y_offset = -5;
    int x_pos = (btn_size - icon_size) / 2 + icon_x_offset;
    int y_pos = (btn_size - icon_size) / 2 + icon_y_offset;
    lv_obj_set_pos(icon, x_pos, y_pos);
    lv_coord_t img_width = menu_items[selected_item_index].icon->header.w;
    lv_coord_t img_height = menu_items[selected_item_index].icon->header.h;
    ESP_LOGD(TAG, "Button size: %d x %d, Set Icon size: %d x %d, Original: %d x %d, Pos: %d, %d\n",
           btn_size, btn_size, icon_size, icon_size, img_width, img_height, x_pos, y_pos);

    if (LV_HOR_RES > 150) {
        lv_obj_t *label = lv_label_create(current_item_obj);
        lv_label_set_text(label, menu_items[selected_item_index].name);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -5);
    }

    // Animate in new item (slide and fade)
    lv_anim_t anim_in;
    lv_anim_init(&anim_in);
    lv_anim_set_var(&anim_in, current_item_obj);
    lv_anim_set_values(&anim_in, start_x, 0);
    lv_anim_set_time(&anim_in, ANIM_DURATION);
    lv_anim_set_path_cb(&anim_in, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&anim_in, anim_set_x);
    lv_anim_start(&anim_in);

    lv_anim_t fade_in;
    lv_anim_init(&fade_in);
    lv_anim_set_var(&fade_in, current_item_obj);
    lv_anim_set_values(&fade_in, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_time(&fade_in, ANIM_DURATION);
    lv_anim_set_exec_cb(&fade_in, anim_set_opa);
    lv_anim_set_ready_cb(&fade_in, fade_in_ready_cb);
    lv_anim_start(&fade_in);

    // Ensure the new item is fully opaque at the end
    lv_obj_set_style_opa(current_item_obj, LV_OPA_COVER, 0); // Always fully opaque
}
/**
 *  @brief handles keyboard button presses
 */

void handle_keyboard_interactions(int keyValue){
    // Support both ASCII and LVGL key codes for h/j/k/l
    if (keyValue == 44 || keyValue == ',' || keyValue == 'h') { // Left or 'h'
        ESP_LOGI(TAG, "Left button or 'h' pressed\n");
        select_menu_item(selected_item_index - 1, true);
    } else if (keyValue == 47 || keyValue == '/' || keyValue == 'l') { // Right or 'l'
        ESP_LOGI(TAG, "Right button or 'l' pressed\n");
        select_menu_item(selected_item_index + 1, false);
    } else if (keyValue == 13 || keyValue == 'j') { // Enter or 'j' (down/select)
        ESP_LOGI(TAG, "Enter button or 'j' pressed\n");
        handle_menu_item_selection(selected_item_index);
    } else if (keyValue == 29 || keyValue == '`' || keyValue == 'k') { // Esc or 'k' (up/escape)
        ESP_LOGI(TAG, "Esc button or 'k' pressed\n");
        // Optionally, implement up/escape action here if needed
    }
}
/**
 * @brief Handles button click events for menu items.
 */
static void menu_button_click_handler(lv_event_t *event) {
    if (current_layout == MENU_LAYOUT_GRID || current_layout == MENU_LAYOUT_GRID_CARDS) {
        int item_index = (int)(intptr_t)lv_event_get_user_data(event);
        if (item_index >= 0 && item_index < num_items) {
            handle_menu_item_selection(item_index);
        }
    }
}

/**
 * @brief Combined handler for menu item events.
 */
static void menu_item_event_handler(InputEvent *event) {
    if (event->type == INPUT_TYPE_TOUCH) {
        ESP_LOGI(TAG, "Touch event");
        lv_indev_data_t *data = &event->data.touch_data;
        if (data->state == LV_INDEV_STATE_PR) {
            touch_started = true;
            touch_start_x = data->point.x;
            touch_start_y = data->point.y;
        } else if (data->state == LV_INDEV_STATE_REL && touch_started) {
            int dx = data->point.x - touch_start_x;
            int dy = data->point.y - touch_start_y;
            touch_started = false;

            // Check if touch was on navigation buttons
            if (left_nav_btn && right_nav_btn) {
                lv_area_t left_area, right_area;
                lv_obj_get_coords(left_nav_btn, &left_area);
                lv_obj_get_coords(right_nav_btn, &right_area);

                // Check if touch point is within left button bounds
                if (data->point.x >= left_area.x1 && data->point.x <= left_area.x2 &&
                    data->point.y >= left_area.y1 && data->point.y <= left_area.y2) {
                    ESP_LOGI(TAG, "Left navigation button touched");
                    animate_nav_button_press(left_nav_btn);
                    select_menu_item(selected_item_index - 1, true);
                    return;
                }

                // Check if touch point is within right button bounds
                if (data->point.x >= right_area.x1 && data->point.x <= right_area.x2 &&
                    data->point.y >= right_area.y1 && data->point.y <= right_area.y2) {
                    ESP_LOGI(TAG, "Right navigation button touched");
                    animate_nav_button_press(right_nav_btn);
                    select_menu_item(selected_item_index + 1, false);
                    return;
                }
            }

            // Handle different layout types
            if (current_layout == MENU_LAYOUT_CAROUSEL) {
                if (abs(dx) > SWIPE_THRESHOLD && abs(dx) > abs(dy)) { // Swipe detected
                    if (dx < 0) {
                        select_menu_item(selected_item_index + 1, true);
                    } else {
                        select_menu_item(selected_item_index - 1, false);
                    }
                } else if (abs(dx) < TAP_THRESHOLD && abs(dy) < TAP_THRESHOLD) { // Tap detected
                    handle_menu_item_selection(selected_item_index);
                }
            } else if (current_layout == MENU_LAYOUT_GRID) {
                // For grid layout, check if tap was on a grid button
                if (abs(dx) < TAP_THRESHOLD && abs(dy) < TAP_THRESHOLD) {
                    // Find which grid button was tapped
                    if (grid_buttons) {
                        for (int i = 0; i < num_items; i++) {
                            if (grid_buttons[i]) {
                                lv_area_t btn_area;
                                lv_obj_get_coords(grid_buttons[i], &btn_area);
                                if (data->point.x >= btn_area.x1 && data->point.x <= btn_area.x2 &&
                                    data->point.y >= btn_area.y1 && data->point.y <= btn_area.y2) {
                                    handle_menu_item_selection(i);
                                    return;
                                }
                            }
                        }
                    }
                }
            } else if (current_layout == MENU_LAYOUT_GRID_CARDS) {
                // For Grid card layout, check if tap was on a card
                if (abs(dx) < TAP_THRESHOLD && abs(dy) < TAP_THRESHOLD) {
                    // Find which card was tapped
                    if (grid_cards) {
                        for (int i = 0; i < num_items; i++) {
                            if (grid_cards[i]) {
                                lv_area_t card_area;
                                lv_obj_get_coords(grid_cards[i], &card_area);
                                if (data->point.x >= card_area.x1 && data->point.x <= card_area.x2 &&
                                    data->point.y >= card_area.y1 && data->point.y <= card_area.y2) {
                                    handle_menu_item_selection(i);
                                    return;
                                }
                            }
                        }
                    }
                }
            }
        }
    } else if (event->type == INPUT_TYPE_JOYSTICK) {
        ESP_LOGI(TAG, "Joystick event");
        int button = event->data.joystick_index;
        handle_hardware_button_press(button);
    } else if (event->type == INPUT_TYPE_ENCODER) {
        if (event->data.encoder.button) {
            handle_menu_item_selection(selected_item_index);
        } else {
            if (event->data.encoder.direction > 0)
                select_menu_item(selected_item_index + 1, false); // CW == right
            else
                select_menu_item(selected_item_index - 1, true);  // CCW == left
        }
    } else if (event->type == INPUT_TYPE_KEYBOARD) {
        ESP_LOGI(TAG, "keyboard event");
        int kv = event->data.key_value;
        if (kv == 13) { // enter key
            handle_menu_item_selection(selected_item_index);
        } else {
            handle_keyboard_interactions(kv);
        }
#ifdef CONFIG_USE_ENCODER
    } else if (event->type == INPUT_TYPE_EXIT_BUTTON) {
        ESP_LOGI(TAG, "IO6 exit button pressed, staying on main menu");
        // On main menu, the exit button doesn't do anything since we're already at the top level
#endif
    }
}

/**
 * @brief Handles hardware button presses for menu navigation.
 */
void handle_hardware_button_press(int ButtonPressed) {
    if (ButtonPressed == 0) {
        select_menu_item(selected_item_index - 1, true);
    } else if (ButtonPressed == 3) {
        select_menu_item(selected_item_index + 1, false);
    } else if (ButtonPressed == 1) {
        handle_menu_item_selection(selected_item_index);
    }
}



/**
 * @brief Selects a menu item and updates the display.
 */
static void select_menu_item(int index, bool slide_left) {
    if (is_animating) return; // Block input during animation
    if (index < 0) index = num_items - 1;
    if (index >= num_items) index = 0;

    // Update selection for different layouts
    if (current_layout == MENU_LAYOUT_CAROUSEL) {
        selected_item_index = index;
        update_menu_item(slide_left);
    } else if (current_layout == MENU_LAYOUT_GRID) {
        // Update selection for grid layout
        if (grid_buttons) {
            // Remove highlight from previous selection
            if (selected_item_index >= 0 && selected_item_index < num_items && grid_buttons[selected_item_index]) {
                lv_obj_set_style_border_width(grid_buttons[selected_item_index], 2, LV_PART_MAIN);
                lv_obj_set_style_border_color(grid_buttons[selected_item_index], lv_color_hex(0x444444), LV_PART_MAIN);
            }

            // Highlight new selection
            selected_item_index = index;
            if (grid_buttons[selected_item_index]) {
                lv_obj_set_style_border_width(grid_buttons[selected_item_index], 4, LV_PART_MAIN);
                lv_obj_set_style_border_color(grid_buttons[selected_item_index], lv_color_hex(0xFFFFFF), LV_PART_MAIN);
            }
        }
    } else if (current_layout == MENU_LAYOUT_GRID_CARDS) {
        // Update selection for Grid card layout
        if (grid_cards) {
            // Remove highlight from previous selection
            if (selected_item_index >= 0 && selected_item_index < num_items && grid_cards[selected_item_index]) {
                // Reset to original styling
                lv_obj_set_style_border_color(grid_cards[selected_item_index], menu_items[selected_item_index].border_color, LV_PART_MAIN);
                lv_obj_set_style_border_width(grid_cards[selected_item_index], 2, LV_PART_MAIN); // Reset to original width
                lv_obj_set_style_shadow_width(grid_cards[selected_item_index], 8, LV_PART_MAIN);
                lv_obj_set_style_shadow_color(grid_cards[selected_item_index], lv_color_hex(0x000000), LV_PART_MAIN);
                lv_obj_set_style_shadow_opa(grid_cards[selected_item_index], LV_OPA_50, LV_PART_MAIN);
            }

            // Highlight new selection
            selected_item_index = index;
            if (grid_cards[selected_item_index]) {
                // For non-touch devices, make highlight more prominent
#ifdef CONFIG_USE_TOUCHSCREEN
                // Touch devices: keep original border color
                lv_obj_set_style_border_color(grid_cards[selected_item_index], menu_items[selected_item_index].border_color, LV_PART_MAIN);
                lv_obj_set_style_shadow_width(grid_cards[selected_item_index], 8, LV_PART_MAIN);
#else
                // Non-touch devices: use prominent white border and larger shadow
                lv_obj_set_style_border_color(grid_cards[selected_item_index], lv_color_hex(0xFFFFFF), LV_PART_MAIN);
                lv_obj_set_style_border_width(grid_cards[selected_item_index], 4, LV_PART_MAIN);
                lv_obj_set_style_shadow_width(grid_cards[selected_item_index], 16, LV_PART_MAIN);
                lv_obj_set_style_shadow_color(grid_cards[selected_item_index], lv_color_hex(0xFFFFFF), LV_PART_MAIN);
                lv_obj_set_style_shadow_opa(grid_cards[selected_item_index], LV_OPA_30, LV_PART_MAIN);
#endif
                // Ensure selected card is visible (handle pagination) without animation
                lv_obj_scroll_to_view(grid_cards[selected_item_index], LV_ANIM_OFF);
            }
        }
    }
}

/**
 * @brief Handles the selection of menu items.
 */
static void handle_menu_item_selection(int item_index) {
    if (is_animating) return;

    typedef struct {
        const char *name;
        EOptionsMenuType type;
        View *view;
    } menu_action_t;

    static const menu_action_t menu_actions[] = {
#ifndef CONFIG_IDF_TARGET_ESP32S2
        {"BLE", OT_Bluetooth, &options_menu_view},
#endif
        {"WiFi", OT_Wifi, &options_menu_view},
#ifdef CONFIG_HAS_GPS
        {"GPS", OT_GPS, &options_menu_view},
#endif
#if CONFIG_HAS_INFRARED
        {"Infrared", 0, &infrared_view},
#endif
#ifdef CONFIG_HAS_NFC
        {"NFC", 0, &nfc_view},
#endif
        {"Apps", 0, &apps_menu_view},
#ifdef CONFIG_HAS_RTC_CLOCK
        {"Clock", 0, &clock_view},
#endif
        {"Settings", OT_Settings, &options_menu_view}
    };

    const int num_actions = sizeof(menu_actions) / sizeof(menu_actions[0]);
    const char *name = menu_items[item_index].name;
    const View *target_view = NULL;
    EOptionsMenuType target_type = 0;
    for (int i = 0; i < num_actions; ++i) {
        if (strcmp(name, menu_actions[i].name) == 0) {
            ESP_LOGI(TAG, "%s selected\n", menu_actions[i].name);
            target_view = menu_actions[i].view;
            target_type = menu_actions[i].type;
            break;
        }
    }
    if (!target_view) {
        ESP_LOGW(TAG, "Unknown menu item selected: %s\n", name);
        return;
    }

    pending_view_to_switch = target_view;
    pending_menu_type = target_type;
    menu_item_selected = true;

    lv_obj_t *anim_target = NULL;
    if (current_layout == MENU_LAYOUT_CAROUSEL && current_item_obj) {
        anim_target = current_item_obj;
    } else if (current_layout == MENU_LAYOUT_GRID_CARDS && grid_cards && item_index >= 0 && item_index < num_items) {
        anim_target = grid_cards[item_index];
    } else if (current_layout == MENU_LAYOUT_GRID && grid_buttons && item_index >= 0 && item_index < num_items) {
        anim_target = grid_buttons[item_index];
    }

    if (anim_target) {
        animate_button_click(anim_target);
    } else {
        if (pending_view_to_switch == &options_menu_view) {
            SelectedMenuType = pending_menu_type;
        }
        display_manager_switch_view((View *)pending_view_to_switch);
        pending_view_to_switch = NULL;
    }
}

/**
 * @brief Creates the menu in Grid-style card layout.
 */
static void create_grid_menu(void) {
    int screen_width = LV_HOR_RES;
    int screen_height = LV_VER_RES;

    // Target ~6 cards visible: 3 columns x 2 rows, horizontal tiles
    int cols = 3;
    int rows = 2;
    int margin = 6; // inner spacing between cards
    int status_bar_height = 20; // reserve space for status bar
    int avail_height = screen_height - status_bar_height;
    if (avail_height < 60) avail_height = screen_height; // safety fallback
    // Fill viewport with 3x2 cards with inner margins between them
    grid_card_width = (screen_width - (cols - 1) * margin) / cols;
    grid_card_height = (avail_height - (rows - 1) * margin) / rows;
    // Reduce margin on very small displays to ensure cards still fit
    if (screen_width <= 240 || avail_height <= 120) {
        margin = 0;
        grid_card_width = (screen_width - (cols - 1) * margin) / cols;
        grid_card_height = (avail_height - (rows - 1) * margin) / rows;
    }

    // Create container for cards (grid-like)
    grid_cards_container = lv_obj_create(menu_container);
    int items_per_page = cols * rows; // 6 per page
    int pages = (num_items + items_per_page - 1) / items_per_page;
    lv_obj_set_size(grid_cards_container, screen_width, avail_height);
    lv_obj_set_style_bg_opa(grid_cards_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid_cards_container, 0, 0);
    lv_obj_set_style_pad_all(grid_cards_container, 0, 0);
    // Top align within menu container so it sits below the status bar
    lv_obj_align(grid_cards_container, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_scrollbar_mode(grid_cards_container, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(grid_cards_container, LV_DIR_HOR);
    // Disable scroll momentum/elastic to keep paging snappy
    lv_obj_clear_flag(grid_cards_container, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_clear_flag(grid_cards_container, LV_OBJ_FLAG_SCROLL_ELASTIC);

    // Allocate cards array
    grid_cards = malloc(num_items * sizeof(lv_obj_t*));
    if (!grid_cards) {
        ESP_LOGE(TAG, "Failed to allocate Grid cards array");
        return;
    }

    // card spacing
    int card_margin = margin;
    // Compute remainders to distribute so we fill the area exactly
    int total_inner_w = cols * grid_card_width + (cols - 1) * card_margin;
    int total_inner_h = rows * grid_card_height + (rows - 1) * card_margin;
    int w_remainder = screen_width - total_inner_w;   // add to last col width
    int h_remainder = (avail_height) - total_inner_h; // add to last row height

    for (int i = 0; i < num_items; i++) {
        // Create card
        grid_cards[i] = lv_btn_create(grid_cards_container);

        // Position card in 3x2 per page; horizontal pages
        int page = i / items_per_page;
        int idx = i % items_per_page;
        int col = idx % cols;
        int row = idx / cols;
        int x = page * screen_width + col * (grid_card_width + card_margin);
        int y = row * (grid_card_height + card_margin); // start at top, no outer gap
        // Per-cell width/height (add remainder to the last column/row)
        int cw = grid_card_width + ((col == cols - 1) ? w_remainder : 0);
        int ch = grid_card_height + ((row == rows - 1) ? h_remainder : 0);
        lv_obj_set_pos(grid_cards[i], x, y);
        lv_obj_set_size(grid_cards[i], cw, ch);

        // Style card (Grid-style with rounded corners and shadows) - use theme colors
        lv_obj_set_style_bg_color(grid_cards[i], lv_color_hex(0x1E1E1E), LV_PART_MAIN);
        int shadow_w = (ch <= 50 ? 4 : 8);
        lv_obj_set_style_shadow_width(grid_cards[i], shadow_w, LV_PART_MAIN);
        lv_obj_set_style_shadow_color(grid_cards[i], lv_color_hex(0x000000), LV_PART_MAIN);
        lv_obj_set_style_shadow_opa(grid_cards[i], LV_OPA_50, LV_PART_MAIN);
        lv_obj_set_style_border_width(grid_cards[i], 2, LV_PART_MAIN);
        lv_obj_set_style_border_color(grid_cards[i], menu_items[i].border_color, LV_PART_MAIN);
        lv_obj_set_style_radius(grid_cards[i], 15, LV_PART_MAIN);
        lv_obj_set_style_pad_all(grid_cards[i], 0, LV_PART_MAIN);

        // Add icon (dynamic sizing to fit with label below)
        lv_obj_t *icon = lv_img_create(grid_cards[i]);
        lv_img_set_src(icon, menu_items[i].icon);
        // Dynamic label reserve to ensure text fits without pushing icon off-screen
        int reserved_for_label = (ch <= 50 ? 14 : 20);
        int avail_w = (int)(cw * 0.78f);
        int avail_h = (int)((ch - reserved_for_label) * 0.78f);
        if (avail_h < 10) avail_h = ch - reserved_for_label;
        lv_img_set_antialias(icon, false);

        // Color icons according to theme like other layouts
        if (strcmp(menu_items[i].name, "Clock")) {
            lv_obj_set_style_img_recolor(icon, menu_items[i].border_color, 0);
            lv_obj_set_style_img_recolor_opa(icon, LV_OPA_COVER, 0);
        }
        lv_obj_set_style_clip_corner(icon, false, 0);

        // Scale icon using zoom to fit into available area
        lv_coord_t img_w = menu_items[i].icon->header.w;
        lv_coord_t img_h = menu_items[i].icon->header.h;
        int zoom_w = (img_w > 0) ? (avail_w * 256) / img_w : 256;
        int zoom_h = (img_h > 0) ? (avail_h * 256) / img_h : 256;
        int zoom = LV_MIN(zoom_w, zoom_h);
        if (zoom > 256) zoom = 256;      // cap at 1x
        if (zoom < 64)  zoom = 64;       // don't get too small
        lv_img_set_zoom(icon, zoom);

        // Place icon above center within the icon area to make room for text below
        int icon_draw_h = (img_h * zoom) / 256;
        int icon_area_h = ch - reserved_for_label;
        int top_offset = (icon_area_h - icon_draw_h) / 2 - (ch <= 50 ? 15 : 18);
        if (top_offset < 0) top_offset = 0;
        lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, top_offset);

        // Add label
        lv_obj_t *label = lv_label_create(grid_cards[i]);
        lv_label_set_text(label, menu_items[i].name);
        // smaller font on small tiles
        const lv_font_t *lbl_font = (ch <= 50 ? &lv_font_montserrat_10 : &lv_font_montserrat_12);
        lv_obj_set_style_text_font(label, lbl_font, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
        // Center label within the card and ensure proper centering of text
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_obj_set_width(label, cw - 8);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -2);

        // Add click event
        lv_obj_add_event_cb(grid_cards[i], menu_button_click_handler, LV_EVENT_CLICKED, (void*)(intptr_t)i);
    }

    // Highlight selected card
    if (grid_cards[selected_item_index]) {
        // For non-touch devices, make highlight more prominent
#ifdef CONFIG_USE_TOUCHSCREEN
        // Touch devices: keep original border color
        lv_obj_set_style_border_color(grid_cards[selected_item_index], menu_items[selected_item_index].border_color, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(grid_cards[selected_item_index], 8, LV_PART_MAIN);
#else
        // Non-touch devices: use prominent white border and larger shadow
        lv_obj_set_style_border_color(grid_cards[selected_item_index], lv_color_hex(0xFFFFFF), LV_PART_MAIN);
        lv_obj_set_style_border_width(grid_cards[selected_item_index], 4, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(grid_cards[selected_item_index], 16, LV_PART_MAIN);
        lv_obj_set_style_shadow_color(grid_cards[selected_item_index], lv_color_hex(0xFFFFFF), LV_PART_MAIN);
        lv_obj_set_style_shadow_opa(grid_cards[selected_item_index], LV_OPA_30, LV_PART_MAIN);
#endif
        lv_obj_scroll_to_view(grid_cards[selected_item_index], LV_ANIM_OFF);
    }

    // horizontal scrolling pages
}


/**
 * @brief Creates the main menu screen view.
 */
void main_menu_create(void) {
    display_manager_fill_screen(lv_color_hex(0x121212));
    init_menu_colors(); // Initialize colors at runtime

    // Set current layout from settings (0 = Normal/Carousel, 1 = Grid)
    uint8_t layout_setting = settings_get_menu_layout(&G_Settings);
    current_layout = (layout_setting == 1) ? MENU_LAYOUT_GRID_CARDS : MENU_LAYOUT_CAROUSEL;

    menu_container = lv_obj_create(lv_scr_act());
    main_menu_view.root = menu_container;
    lv_obj_set_size(menu_container, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_opa(menu_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(menu_container, 0, 0);
    lv_obj_set_style_pad_all(menu_container, 0, 0);
    lv_obj_set_scrollbar_mode(menu_container, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(menu_container, LV_ALIGN_CENTER, 0, 0);

    // Create menu based on layout
    if (current_layout == MENU_LAYOUT_GRID) {
        create_grid_menu();
    } else if (current_layout == MENU_LAYOUT_GRID_CARDS) {
        create_grid_menu();
    } else {
        // Default carousel layout
        update_menu_item(false);
    }

    // Check if navigation buttons should be shown based on user setting
    // Also respect the original logic for device capabilities
    bool should_show_nav_buttons = settings_get_nav_buttons_enabled(&G_Settings);

    // Only show if both user wants them AND device supports them AND not grid layout
    if (should_show_nav_buttons) {
#ifdef CONFIG_LVGL_TOUCH
        should_show_nav_buttons = true;
#else
        // Check screen dimensions at runtime
        int screen_width = lv_disp_get_hor_res(lv_disp_get_default());
        should_show_nav_buttons = (screen_width > 200);
#endif
    }

    // Don't show navigation buttons for grid layout since cards are clickable
    if (should_show_nav_buttons && current_layout == MENU_LAYOUT_GRID_CARDS) {
        should_show_nav_buttons = false;
    }

    if (should_show_nav_buttons) {
        // Create left navigation button
        left_nav_btn = lv_btn_create(lv_scr_act());
        
        // Responsive button sizing based on screen dimensions - make them smaller
        int btn_size = 52; // Default slightly larger size
        int btn_margin = 15;
        int screen_width = lv_disp_get_hor_res(lv_disp_get_default());
        if (screen_width <= 128) {
            btn_size = 40; // smaller for small screens
            btn_margin = 10;
        } else if (screen_width >= 320) {
            btn_size = 60; // larger for large screens
            btn_margin = 20;
        }
        
        lv_obj_set_size(left_nav_btn, btn_size, btn_size);
        // make button transparent and remove shadows/border
        lv_obj_set_style_bg_opa(left_nav_btn, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(left_nav_btn, 0, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(left_nav_btn, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(left_nav_btn, 0, LV_PART_MAIN);
        
        // Position left button vertically centered at left edge
        lv_obj_align(left_nav_btn, LV_ALIGN_LEFT_MID, btn_margin, 0);
        
        // Add left arrow icon/text
        // create arrow label only, style it and center within the transparent button
        lv_obj_t *left_label = lv_label_create(left_nav_btn);
        lv_label_set_text(left_label, "<");
        // increase arrow size for better visibility
        lv_obj_set_style_text_font(left_label, &lv_font_montserrat_18, 0);
        if (btn_size < 40) {
            lv_obj_set_style_text_font(left_label, &lv_font_montserrat_14, 0);
        }
        lv_obj_set_style_text_color(left_label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_align(left_label, LV_ALIGN_CENTER, 0, 0);

        // Create right navigation button
        right_nav_btn = lv_btn_create(lv_scr_act());
        lv_obj_set_size(right_nav_btn, btn_size, btn_size);
        // make button transparent and remove shadows/border
        lv_obj_set_style_bg_opa(right_nav_btn, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(right_nav_btn, 0, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(right_nav_btn, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(right_nav_btn, 0, LV_PART_MAIN);
        
        // Position right button vertically centered at right edge
        lv_obj_align(right_nav_btn, LV_ALIGN_RIGHT_MID, -btn_margin, 0);
        
        // Add right arrow icon/text
        lv_obj_t *right_label = lv_label_create(right_nav_btn);
        lv_label_set_text(right_label, ">");
        // increase arrow size for better visibility
        lv_obj_set_style_text_font(right_label, &lv_font_montserrat_18, 0);
        if (btn_size < 40) {
            lv_obj_set_style_text_font(right_label, &lv_font_montserrat_14, 0);
        }
        lv_obj_set_style_text_color(right_label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_align(right_label, LV_ALIGN_CENTER, 0, 0);
        
        ESP_LOGI(TAG, "Navigation buttons created - size: %d, margin: %d", btn_size, btn_margin);
    }

    display_manager_add_status_bar(LV_HOR_RES > 128 ? "Main Menu" : "");

    // Position the menu relative to the status bar
    int status_bar_height = 20; // set in display_manager_add_status_bar()
    if (menu_container) {
        if (current_layout == MENU_LAYOUT_GRID_CARDS) {
        // Position directly below the status bar with no extra gap
        lv_obj_align(menu_container, LV_ALIGN_TOP_MID, 0, status_bar_height);
        // Also reduce container height so there is no bottom gap
        lv_obj_set_size(menu_container, LV_HOR_RES, LV_VER_RES - status_bar_height);
    } else {
        // Center for carousel/grid
        lv_obj_align(menu_container, LV_ALIGN_CENTER, 0, status_bar_height / 2);
    }
    }

    // also shift nav buttons down so they remain vertically centered with the menu
    if (left_nav_btn) {
        lv_coord_t old_y = lv_obj_get_y(left_nav_btn);
        lv_obj_set_y(left_nav_btn, old_y + status_bar_height / 2);
    }
    if (right_nav_btn) {
        lv_coord_t old_y = lv_obj_get_y(right_nav_btn);
        lv_obj_set_y(right_nav_btn, old_y + status_bar_height / 2);
    }
}

/**
 * @brief Destroys the main menu screen view.
 */
void main_menu_destroy(void) {
    if (menu_container) {
        lv_obj_clean(menu_container);
        lv_obj_del(menu_container);
        menu_container = NULL;
        main_menu_view.root = NULL;
        current_item_obj = NULL;
        // Children (including Grid container/buttons) are deleted with parent
        grid_cards_container = NULL;
    }

    // Clean up grid layout
    if (grid_buttons) {
        free(grid_buttons);
        grid_buttons = NULL;
    }

    // Clean up Grid layout array; container already deleted with parent
    if (grid_cards) {
        free(grid_cards);
        grid_cards = NULL;
    }

    // Clean up navigation buttons
    if (left_nav_btn) {
        lv_obj_del(left_nav_btn);
        left_nav_btn = NULL;
    }
    if (right_nav_btn) {
        lv_obj_del(right_nav_btn);
        right_nav_btn = NULL;
    }
}

void get_main_menu_callback(void **callback) {
    *callback = main_menu_view.input_callback;
}



View main_menu_view = {
    .root = NULL,
    .create = main_menu_create,
    .destroy = main_menu_destroy,
    .input_callback = menu_item_event_handler,
    .name = "Main Menu",
    .get_hardwareinput_callback = get_main_menu_callback, // Corrected typo
};