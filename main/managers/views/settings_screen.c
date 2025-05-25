#include "managers/views/settings_screen.h"
#include "managers/views/main_menu_screen.h"
#include <stdio.h>

#define SCROLL_BTN_SIZE 40
#define SCROLL_BTN_PADDING 5

// Settings screen UI elements
static lv_obj_t *root_container;
static lv_obj_t *menu_container;
static lv_obj_t *scroll_up_btn;
static lv_obj_t *scroll_down_btn;
static lv_obj_t *back_btn;
static lv_obj_t *setting_btns[6];
static int current_option_indices[6];
static bool touch_started = false;
static int touch_start_x;
static int touch_start_y;
static int selected_setting = 0; // index of currently selected setting

static const char *setting_labels[] = {"Display Timeout", "RGB Mode", "Menu Theme", "Old Controls", "Terminal Text Color", "Invert Colors"};
static const char *timeout_options[] = {"5s", "10s", "30s", "60s"};
static const char *rgb_options[] = {"Normal", "Rainbow"};
static const char *theme_options[] = {"Default", "Pastel", "Dark", "Bright", "Solarized", "Monochrome", "Rose Red", "Purple", "Blue", "Orange", "Neon", "Cyberpunk", "Ocean", "Sunset", "Forest"};
static const char *third_options[] = {"Off", "On"};
static const char *textcolor_labels[] = {"Green", "White", "Red", "Blue", "Yellow", "Cyan", "Magenta", "Orange"};
static const uint32_t textcolor_values[] = {0x00FF00, 0xFFFFFF, 0xFF0000, 0x0000FF, 0xFFFF00, 0x00FFFF, 0xFF00FF, 0xFFA500};
static const char *invert_options[] = {"Off", "On"};

typedef struct SettingsMenuItem {
    const char *label;
    int setting_idx;
    struct SettingsMenuItem *submenu;
    int submenu_count;
} SettingsMenuItem;

// Forward declarations for dynamic menu functions
static void populate_menu(SettingsMenuItem *items, int count);
static void pop_menu(void);
static void menu_item_cb(lv_event_t *e);

static SettingsMenuItem display_menu[] = {
    {"Display Timeout", 0, NULL, 0},
    {"Menu Theme", 2, NULL, 0},
    {"Invert Colors", 5, NULL, 0}
};

static SettingsMenuItem config_menu[] = {
    {"RGB Mode", 1, NULL, 0},
    {"Old Controls", 3, NULL, 0},
    {"Terminal Text Color", 4, NULL, 0}
};

static SettingsMenuItem root_menu[] = {
    {"Display", -1, display_menu, sizeof(display_menu)/sizeof(display_menu[0])},
    {"Config", -1, config_menu, sizeof(config_menu)/sizeof(config_menu[0])}
};

static struct {
    SettingsMenuItem *items;
    int count;
} menu_stack[10];
static int menu_stack_top = 0;
static lv_obj_t *menu_buttons[20];
static int menu_button_count = 0;
static int selected_menu_idx = 0;
static int menu_button_height = 0;
static bool menu_is_small = false;

static void scroll_up_cb(lv_event_t *e) {
    if (!menu_container) return;
    lv_coord_t scroll_amt = lv_obj_get_height(menu_container) / 2;
    lv_obj_scroll_by_bounded(menu_container, 0, -scroll_amt, LV_ANIM_OFF);
}

static void scroll_down_cb(lv_event_t *e) {
    if (!menu_container) return;
    lv_coord_t scroll_amt = lv_obj_get_height(menu_container) / 2;
    lv_obj_scroll_by_bounded(menu_container, 0, scroll_amt, LV_ANIM_OFF);
}

static void change_setting(int idx, bool inc) {
    int max;
    if (idx == 0) max = sizeof(timeout_options)/sizeof(timeout_options[0]);
    else if (idx == 1) max = sizeof(rgb_options)/sizeof(rgb_options[0]);
    else if (idx == 2) max = sizeof(theme_options)/sizeof(theme_options[0]);
    else if (idx == 3) max = sizeof(third_options)/sizeof(third_options[0]);
    else if (idx == 4) max = sizeof(textcolor_labels)/sizeof(textcolor_labels[0]);
    else if (idx == 5) max = sizeof(invert_options)/sizeof(invert_options[0]);
    else return;
    if (inc) {
        current_option_indices[idx] = (current_option_indices[idx] + 1) % max;
    } else {
        current_option_indices[idx] = (current_option_indices[idx] + max - 1) % max;
    }
    if (idx == 0) {
        uint32_t v = current_option_indices[idx] == 0 ? 5000 :
                     current_option_indices[idx] == 1 ? 10000 :
                     current_option_indices[idx] == 2 ? 30000 : 60000;
        settings_set_display_timeout(&G_Settings, v);
    } else if (idx == 1) {
        settings_set_rgb_mode(&G_Settings, current_option_indices[idx]);
    } else if (idx == 2) {
        settings_set_menu_theme(&G_Settings, current_option_indices[idx]);
    } else if (idx == 3) {
        settings_set_thirds_control_enabled(&G_Settings, current_option_indices[idx] == 1);
    } else if (idx == 4) {
        settings_set_terminal_text_color(&G_Settings, textcolor_values[current_option_indices[idx]]);
    } else if (idx == 5) {
        settings_set_invert_colors(&G_Settings, current_option_indices[idx] == 1);
    }
    settings_save(&G_Settings);
    char buf[64];
    const char *val;
    if (idx == 0) val = timeout_options[current_option_indices[idx]];
    else if (idx == 1) val = rgb_options[current_option_indices[idx]];
    else if (idx == 2) val = theme_options[current_option_indices[idx]];
    else if (idx == 3) val = third_options[current_option_indices[idx]];
    else if (idx == 4) val = textcolor_labels[current_option_indices[idx]];
    else if (idx == 5) val = invert_options[current_option_indices[idx]];
    snprintf(buf, sizeof(buf), "%s %s: %s %s", LV_SYMBOL_LEFT, setting_labels[idx], val, LV_SYMBOL_RIGHT);
    lv_obj_t *btn = setting_btns[idx];
    lv_obj_t *label = lv_obj_get_child(btn, 0);
    lv_label_set_text(label, buf);
    lv_obj_set_width(label, lv_obj_get_width(btn));
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_invalidate(lv_scr_act());
    populate_menu(menu_stack[menu_stack_top].items, menu_stack[menu_stack_top].count);
}

static void setting_row_cb(lv_event_t *e) {
    lv_obj_t *btn = lv_event_get_target(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(btn);
    int max;
    if (idx == 0) max = sizeof(timeout_options)/sizeof(timeout_options[0]);
    else if (idx == 1) max = sizeof(rgb_options)/sizeof(rgb_options[0]);
    else if (idx == 2) max = sizeof(theme_options)/sizeof(theme_options[0]);
    else if (idx == 3) max = sizeof(third_options)/sizeof(third_options[0]);
    else if (idx == 4) max = sizeof(textcolor_labels)/sizeof(textcolor_labels[0]);
    else if (idx == 5) max = sizeof(invert_options)/sizeof(invert_options[0]);
    else return;
    current_option_indices[idx] = (current_option_indices[idx] + 1) % max;
    if (idx == 0) {
        uint32_t v = current_option_indices[idx] == 0 ? 5000 : current_option_indices[idx] == 1 ? 10000 : current_option_indices[idx] == 2 ? 30000 : 60000;
        settings_set_display_timeout(&G_Settings, v);
    } else if (idx == 1) {
        settings_set_rgb_mode(&G_Settings, current_option_indices[idx]);
    } else if (idx == 2) {
        settings_set_menu_theme(&G_Settings, current_option_indices[idx]);
    } else if (idx == 3) {
        settings_set_thirds_control_enabled(&G_Settings, current_option_indices[idx] == 1);
    } else if (idx == 4) {
        settings_set_terminal_text_color(&G_Settings, textcolor_values[current_option_indices[idx]]);
    } else if (idx == 5) {
        settings_set_invert_colors(&G_Settings, current_option_indices[idx] == 1);
    }
    settings_save(&G_Settings);
    char buf[64];
    const char *val;
    if (idx == 0) val = timeout_options[current_option_indices[idx]];
    else if (idx == 1) val = rgb_options[current_option_indices[idx]];
    else if (idx == 2) val = theme_options[current_option_indices[idx]];
    else if (idx == 3) val = third_options[current_option_indices[idx]];
    else if (idx == 4) val = textcolor_labels[current_option_indices[idx]];
    else if (idx == 5) val = invert_options[current_option_indices[idx]];
    snprintf(buf, sizeof(buf), "%s %s: %s %s", LV_SYMBOL_LEFT, setting_labels[idx], val, LV_SYMBOL_RIGHT);
    lv_obj_t *label = lv_obj_get_child(btn, 0);
    lv_label_set_text(label, buf);
    lv_obj_set_width(label, lv_obj_get_width(btn));
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_invalidate(lv_scr_act());
    populate_menu(menu_stack[menu_stack_top].items, menu_stack[menu_stack_top].count);
}

static void back_button_cb(lv_event_t *e) {
    if (menu_stack_top > 0) pop_menu();
    else display_manager_switch_view(&main_menu_view);
}

static void event_handler(InputEvent *ev) {
    if (ev->type == INPUT_TYPE_TOUCH) {
        lv_indev_data_t *data = &ev->data.touch_data;
        if (data->state == LV_INDEV_STATE_PR) {
            touch_started = true;
            touch_start_x = data->point.x;
            touch_start_y = data->point.y;
        } else if (data->state == LV_INDEV_STATE_REL && touch_started) {
            int x = data->point.x;
            int y = data->point.y;
            touch_started = false;
            lv_area_t area;
            for (int i = 0; i < menu_button_count; i++) {
                if (menu_buttons[i]) {
                    lv_obj_get_coords(menu_buttons[i], &area);
                    if (x >= area.x1 && x <= area.x2 && y >= area.y1 && y <= area.y2) {
                        lv_event_send(menu_buttons[i], LV_EVENT_CLICKED, NULL);
                        return;
                    }
                }
            }
            if (scroll_up_btn) {
                lv_obj_get_coords(scroll_up_btn, &area);
                if (x >= area.x1 && x <= area.x2 && y >= area.y1 && y <= area.y2) {
                    lv_event_send(scroll_up_btn, LV_EVENT_CLICKED, NULL);
                    return;
                }
            }
            if (scroll_down_btn) {
                lv_obj_get_coords(scroll_down_btn, &area);
                if (x >= area.x1 && x <= area.x2 && y >= area.y1 && y <= area.y2) {
                    lv_event_send(scroll_down_btn, LV_EVENT_CLICKED, NULL);
                    return;
                }
            }
            if (back_btn) {
                lv_obj_get_coords(back_btn, &area);
                if (x >= area.x1 && x <= area.x2 && y >= area.y1 && y <= area.y2) {
                    lv_event_send(back_btn, LV_EVENT_CLICKED, NULL);
                    return;
                }
            }
        }
    } else if (ev->type == INPUT_TYPE_JOYSTICK) {
        int b = ev->data.joystick_index;
        // Up arrow -> previous setting
        if (b == 1) {
            lv_obj_clear_state(setting_btns[selected_setting], LV_STATE_FOCUSED);
            selected_setting = (selected_setting + 6 - 1) % 6;
            lv_obj_add_state(setting_btns[selected_setting], LV_STATE_FOCUSED);
            lv_obj_scroll_to_view(setting_btns[selected_setting], LV_ANIM_OFF);
        }
        // Down arrow -> next setting
        else if (b == 4) {
            lv_obj_clear_state(setting_btns[selected_setting], LV_STATE_FOCUSED);
            selected_setting = (selected_setting + 1) % 6;
            lv_obj_add_state(setting_btns[selected_setting], LV_STATE_FOCUSED);
            lv_obj_scroll_to_view(setting_btns[selected_setting], LV_ANIM_OFF);
        }
        // Left arrow -> decrement value of selected setting
        else if (b == 0) {
            change_setting(selected_setting, false);
        }
        // Right arrow -> increment value of selected setting
        else if (b == 3) {
            change_setting(selected_setting, true);
        }
        // Back -> exit settings
        else if (b == 2) {
            lv_event_send(back_btn, LV_EVENT_CLICKED, NULL);
        }
    }
}

void settings_screen_create(void) {
    uint32_t dt = settings_get_display_timeout(&G_Settings);
    current_option_indices[0] = dt < 7500 ? 0 : dt < 15000 ? 1 : dt < 45000 ? 2 : 3;
    current_option_indices[1] = settings_get_rgb_mode(&G_Settings);
    current_option_indices[2] = settings_get_menu_theme(&G_Settings);
    current_option_indices[3] = settings_get_thirds_control_enabled(&G_Settings) ? 1 : 0;
    current_option_indices[4] = 0;
    for (int i = 0; i < sizeof(textcolor_values)/sizeof(textcolor_values[0]); i++) {
        if (settings_get_terminal_text_color(&G_Settings) == textcolor_values[i]) {
            current_option_indices[4] = i;
            break;
        }
    }
    current_option_indices[5] = settings_get_invert_colors(&G_Settings) ? 1 : 0;

    display_manager_fill_screen(lv_color_hex(0x121212));
    root_container = lv_obj_create(lv_scr_act());
    settings_screen_view.root = root_container;
    lv_obj_set_size(root_container, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(root_container, lv_color_hex(0x121212), 0);
    lv_obj_set_style_bg_opa(root_container, LV_OPA_COVER, 0);
    lv_obj_align(root_container, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_clear_flag(root_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_width(root_container, 0, 0);
    lv_obj_set_style_pad_all(root_container, 0, 0);
    lv_obj_set_scrollbar_mode(root_container, LV_SCROLLBAR_MODE_OFF);

    int screen_w = LV_HOR_RES;
    int screen_h = LV_VER_RES;
    bool is_small = (screen_w <= 240 || screen_h <= 240);
    int button_height = is_small ? 40 : 60;
    const int STATUS_BAR_H = 20;
    const int BUTTON_AREA_HEIGHT = SCROLL_BTN_SIZE + SCROLL_BTN_PADDING * 2;
    int list_h = screen_h - STATUS_BAR_H - BUTTON_AREA_HEIGHT;

    menu_container = lv_list_create(root_container);
    lv_obj_set_size(menu_container, screen_w, list_h);
    lv_obj_align(menu_container, LV_ALIGN_TOP_MID, 0, STATUS_BAR_H);
    lv_obj_set_style_bg_color(menu_container, lv_color_hex(0x121212), 0);
    lv_obj_set_style_pad_all(menu_container, 0, 0);
    lv_obj_set_style_border_width(menu_container, 0, 0);
    lv_obj_set_scrollbar_mode(menu_container, LV_SCROLLBAR_MODE_OFF);

    menu_is_small = is_small;
    menu_button_height = button_height;
    menu_stack_top = 0;
    menu_stack[0].items = root_menu;
    menu_stack[0].count = sizeof(root_menu)/sizeof(root_menu[0]);
    populate_menu(menu_stack[0].items, menu_stack[0].count);

    // Scroll up
    scroll_up_btn = lv_btn_create(root_container);
    lv_obj_set_size(scroll_up_btn, SCROLL_BTN_SIZE, SCROLL_BTN_SIZE);
    lv_obj_align(scroll_up_btn, LV_ALIGN_BOTTOM_RIGHT, -SCROLL_BTN_PADDING, -SCROLL_BTN_PADDING);
    lv_obj_set_style_bg_color(scroll_up_btn, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_radius(scroll_up_btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_border_width(scroll_up_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(scroll_up_btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(scroll_up_btn, scroll_up_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ul = lv_label_create(scroll_up_btn);
    lv_label_set_text(ul, LV_SYMBOL_UP);
    lv_obj_center(ul);

    // Scroll down
    scroll_down_btn = lv_btn_create(root_container);
    lv_obj_set_size(scroll_down_btn, SCROLL_BTN_SIZE, SCROLL_BTN_SIZE);
    lv_obj_align(scroll_down_btn, LV_ALIGN_BOTTOM_LEFT, SCROLL_BTN_PADDING, -SCROLL_BTN_PADDING);
    lv_obj_set_style_bg_color(scroll_down_btn, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_radius(scroll_down_btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_border_width(scroll_down_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(scroll_down_btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(scroll_down_btn, scroll_down_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *dl = lv_label_create(scroll_down_btn);
    lv_label_set_text(dl, LV_SYMBOL_DOWN);
    lv_obj_center(dl);

    // Back button
    back_btn = lv_btn_create(root_container);
    lv_obj_set_size(back_btn, SCROLL_BTN_SIZE + 20, SCROLL_BTN_SIZE);
    lv_obj_align(back_btn, LV_ALIGN_BOTTOM_MID, 0, -SCROLL_BTN_PADDING);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0x555555), LV_PART_MAIN);
    lv_obj_set_style_radius(back_btn, 5, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(back_btn, 10, LV_PART_MAIN);
    lv_obj_set_style_border_width(back_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(back_btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(back_btn, back_button_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(back_btn);
    lv_label_set_text(bl, LV_SYMBOL_LEFT " Back");
    lv_obj_center(bl);

    display_manager_add_status_bar("Settings");
}

void settings_screen_destroy(void) {
    if (settings_screen_view.root) {
        lv_obj_clean(settings_screen_view.root);
        lv_obj_del(settings_screen_view.root);
        settings_screen_view.root = NULL;
        scroll_up_btn = NULL;
        scroll_down_btn = NULL;
        back_btn = NULL;
    }
}

void get_settings_screen_callback(void **cb) {
    *cb = event_handler;
}

View settings_screen_view = {
    .root = NULL,
    .create = settings_screen_create,
    .destroy = settings_screen_destroy,
    .input_callback = event_handler,
    .name = "Settings",
    .get_hardwareinput_callback = get_settings_screen_callback
};

// Insert dynamic menu support functions
static void populate_menu(SettingsMenuItem *items, int count) {
    menu_button_count = 0;
    lv_obj_clean(menu_container);
    for (int i = 0; i < count; i++) {
        SettingsMenuItem *item = &items[i];
        char buf[64];
        if (item->setting_idx >= 0) {
            const char *val;
            int idx = item->setting_idx;
            if (idx == 0) val = timeout_options[current_option_indices[idx]];
            else if (idx == 1) val = rgb_options[current_option_indices[idx]];
            else if (idx == 2) val = theme_options[current_option_indices[idx]];
            else if (idx == 3) val = third_options[current_option_indices[idx]];
            else if (idx == 4) val = textcolor_labels[current_option_indices[idx]];
            else if (idx == 5) val = invert_options[current_option_indices[idx]];
            else val = "";
            snprintf(buf, sizeof(buf), "%s %s: %s %s", LV_SYMBOL_LEFT, item->label, val, LV_SYMBOL_RIGHT);
        } else {
            snprintf(buf, sizeof(buf), "%s %s", item->label, LV_SYMBOL_RIGHT);
        }
        lv_obj_t *btn = lv_list_add_btn(menu_container, NULL, buf);
        lv_obj_set_height(btn, menu_button_height);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x1E1E1E), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x444444), LV_PART_MAIN | LV_STATE_FOCUSED);
        lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(btn, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_top(btn, SCROLL_BTN_PADDING * 2, LV_PART_MAIN);
        lv_obj_set_style_pad_bottom(btn, SCROLL_BTN_PADDING * 2, LV_PART_MAIN);
        lv_obj_t *label = lv_obj_get_child(btn, 0);
        lv_obj_set_style_text_font(label, menu_is_small ? &lv_font_montserrat_14 : &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_width(label, lv_obj_get_width(btn));
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_user_data(btn, item);
        lv_obj_add_event_cb(btn, menu_item_cb, LV_EVENT_CLICKED, NULL);
        if(item->setting_idx >= 0) setting_btns[item->setting_idx] = btn;
        menu_buttons[menu_button_count++] = btn;
    }
    selected_menu_idx = 0;
    for (int i = 0; i < menu_button_count; i++) {
        if (i == selected_menu_idx) lv_obj_add_state(menu_buttons[i], LV_STATE_FOCUSED);
        else lv_obj_clear_state(menu_buttons[i], LV_STATE_FOCUSED);
    }
    if (menu_button_count > 0) lv_obj_scroll_to_view(menu_buttons[selected_menu_idx], LV_ANIM_OFF);
}

static void pop_menu(void) {
    if (menu_stack_top > 0) {
        menu_stack_top--;
        populate_menu(menu_stack[menu_stack_top].items, menu_stack[menu_stack_top].count);
    }
}

static void menu_item_cb(lv_event_t *e) {
    lv_obj_t *btn = lv_event_get_target(e);
    SettingsMenuItem *item = lv_obj_get_user_data(btn);
    if (item->submenu) {
        if (menu_stack_top < 9) {
            menu_stack[++menu_stack_top].items = item->submenu;
            menu_stack[menu_stack_top].count = item->submenu_count;
        }
        populate_menu(item->submenu, item->submenu_count);
    } else if (item->setting_idx >= 0) {
        change_setting(item->setting_idx, true);
    }
} 