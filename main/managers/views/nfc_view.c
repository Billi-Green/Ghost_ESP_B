// new nfc_view.c - simple NFC view based on options_screen layout
#include "managers/display_manager.h"
#include "managers/views/main_menu_screen.h"
#include "managers/settings_manager.h"
#include "lvgl.h"
#include "esp_log.h"
#include <string.h>

// Forward declaration of this view instance for internal references
extern View nfc_view;

static const char *TAG = "NFCView";

// touch nav button sizing to match options_screen
#define SCROLL_BTN_SIZE 40
#define SCROLL_BTN_PADDING 5

static lv_style_t style_menu_item;
static lv_style_t style_menu_item_alt;
static lv_style_t style_selected_item;
static lv_style_t style_menu_label;
static bool styles_initialized = false;

static lv_obj_t *root = NULL;
static lv_obj_t *menu_container = NULL;
static lv_obj_t *scan_btn = NULL;
static lv_obj_t *emulate_btn = NULL;
static lv_obj_t *scroll_up_btn = NULL;
static lv_obj_t *scroll_down_btn = NULL;
static lv_obj_t *back_btn = NULL;
static int selected_index = 0;
static int num_items = 0; // will be set when building menu

// Match options_screen theme palettes for selected row color
static const uint32_t theme_palettes[15][6] = {
    {0x1976D2,0xD32F2F,0x388E3C,0x7B1FA2,0x000000,0xFF9800},
    {0xFFCDD2,0xC8E6C9,0xB3E5FC,0xFFF9C4,0xD1C4E9,0xCFD8DC},
    {0x263238,0x37474F,0x455A64,0x546E7A,0x263238,0x37474F},
    {0xFFFFFF,0xFFFFFF,0xFFFFFF,0xFFFFFF,0xFFFFFF,0xFFFFFF},
    {0x002B36,0x073642,0x586E75,0x839496,0xEEE8D5,0x002B36},
    {0x888888,0x888888,0x888888,0x888888,0x888888,0x888888},
    {0xE91E63,0xE91E63,0xE91E63,0xE91E63,0xE91E63,0xE91E63},
    {0x9C27B0,0x9C27B0,0x9C27B0,0x9C27B0,0x9C27B0,0x9C27B0},
    {0x2196F3,0x2196F3,0x2196F3,0x2196F3,0x2196F3,0x2196F3},
    {0xFFA500,0xFFA500,0xFFA500,0xFFA500,0xFFA500,0xFFA500},
    {0x39FF14,0xFF073A,0x0FF1CE,0xF8F32B,0xFF6EC7,0xFF8C00},
    {0xFF00FF,0x00FFFF,0xFF0000,0x00FF00,0xFFFF00,0x800080},
    {0x0077BE,0x00CED1,0x20B2AA,0x4682B4,0x5F9EA0,0x00008B},
    {0xFF4500,0xFF8C00,0xFFD700,0xFF1493,0x8B008B,0x2E0854},
    {0x556B2F,0x6B8E23,0x228B22,0x2E8B57,0x8FBC8F,0x8B4513}
};

static int button_height_global = 0;
static bool is_small_screen_global = false;

// NFC scan popup (modeled after IR learning popup)
static lv_obj_t *nfc_scan_popup = NULL;
static lv_obj_t *nfc_scan_cancel_btn = NULL;
static lv_obj_t *nfc_uid_label = NULL;
static lv_obj_t *nfc_type_label = NULL;
static int nfc_popup_selected = 0; // 0 = cancel (only one option for now)
static void nfc_scan_cancel_cb(lv_event_t *e);
static void create_nfc_scan_popup(void);
static void cleanup_nfc_scan_popup(void *obj);
static void update_nfc_popup_selection(void);

static void update_selected_style_from_theme(void) {
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    lv_color_t theme_bg = lv_color_hex(theme_palettes[theme][0]);
    lv_style_set_bg_color(&style_selected_item, theme_bg);
    lv_style_set_bg_grad_dir(&style_selected_item, LV_GRAD_DIR_NONE);
    lv_style_set_bg_grad_color(&style_selected_item, theme_bg);
}

static void highlight_selected(void) {
    if (!menu_container) return;
    for (int i = 0; i < num_items; ++i) {
        lv_obj_t *child = lv_obj_get_child(menu_container, i);
        if (!child) continue;
        lv_obj_t *label = lv_obj_get_child(child, 0);
        if (i == selected_index) {
            update_selected_style_from_theme();
            lv_obj_add_style(child, &style_selected_item, 0);
            if (label) {
                uint8_t theme = settings_get_menu_theme(&G_Settings);
                if (theme == 3) lv_obj_set_style_text_color(label, lv_color_hex(0x000000), 0);
                else lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
            }
            lv_obj_scroll_to_view(child, LV_ANIM_OFF);
        } else {
            lv_obj_remove_style(child, &style_selected_item, 0);
            if (label) lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
        }
    }
}

static void execute_selected(void) { /* no bottom status text in this view */ }

static void init_styles(void) {
    if (styles_initialized) return;
    lv_style_init(&style_menu_item);
    lv_style_set_bg_color(&style_menu_item, lv_color_hex(0x1E1E1E));
    lv_style_set_bg_opa(&style_menu_item, LV_OPA_COVER);
    lv_style_set_border_width(&style_menu_item, 0);
    lv_style_set_radius(&style_menu_item, 0);

    lv_style_init(&style_menu_item_alt);
    lv_style_set_bg_color(&style_menu_item_alt, lv_color_hex(0x232323));
    lv_style_set_bg_opa(&style_menu_item_alt, LV_OPA_COVER);
    lv_style_set_border_width(&style_menu_item_alt, 0);
    lv_style_set_radius(&style_menu_item_alt, 0);

    lv_style_init(&style_selected_item);
    lv_style_set_bg_opa(&style_selected_item, LV_OPA_COVER);
    lv_style_set_radius(&style_selected_item, 0);

    lv_style_init(&style_menu_label);
    lv_style_set_text_color(&style_menu_label, lv_color_hex(0xFFFFFF));

    styles_initialized = true;
}

static const lv_font_t* get_menu_font(void) { return is_small_screen_global ? &lv_font_montserrat_12 : &lv_font_montserrat_14; }

static void vertically_center_label(lv_obj_t *label, lv_obj_t *btn) {
    if (!label || !btn) return;
    lv_obj_set_style_pad_top(btn, 0, 0);
    float btn_y_center_pad = (button_height_global - lv_font_get_line_height(get_menu_font())) / 2;
    if (btn_y_center_pad < 0) btn_y_center_pad = 0;
    lv_obj_set_style_pad_top(label, btn_y_center_pad, 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
}

// forward declare back_event_cb so it can be used before its definition
static void back_event_cb(lv_event_t *e);
// forward declare option dispatcher used by multiple input paths
static void nfc_option_event_cb(lv_event_t *e);

static void nfc_view_input_cb(InputEvent *event) {
    if (!root) return;
    // Handle NFC scan popup input first
    if (nfc_scan_popup && lv_obj_is_valid(nfc_scan_popup)) {
        if (event->type == INPUT_TYPE_TOUCH) {
            lv_indev_data_t *d = &event->data.touch_data;
            if (d->state == LV_INDEV_STATE_PR) return;
            if (nfc_scan_cancel_btn && lv_obj_is_valid(nfc_scan_cancel_btn)) {
                lv_area_t a; lv_obj_get_coords(nfc_scan_cancel_btn, &a);
                if (d->point.x >= a.x1 && d->point.x <= a.x2 && d->point.y >= a.y1 && d->point.y <= a.y2) {
                    nfc_scan_cancel_cb(NULL);
                    return;
                }
            }
            update_nfc_popup_selection();
        } else if (event->type == INPUT_TYPE_JOYSTICK) {
            if (event->data.joystick_index == 2 || event->data.joystick_index == 4) {
                // up/down would toggle selection if more buttons; keep highlight refreshed
                update_nfc_popup_selection();
            } else if (event->data.joystick_index == 1 || event->data.joystick_index == 0) { // select or back
                nfc_scan_cancel_cb(NULL);
                return;
            }
        } else if (event->type == INPUT_TYPE_ENCODER) {
            if (event->data.encoder.button) { nfc_scan_cancel_cb(NULL); return; }
            // rotation: refresh highlight (only one option to select)
            update_nfc_popup_selection();
        } else if (event->type == INPUT_TYPE_KEYBOARD) {
            if (event->data.key_value == 13 || event->data.key_value == 10 || event->data.key_value == 27) { // Enter/Esc
                nfc_scan_cancel_cb(NULL);
                return;
            }
            update_nfc_popup_selection();
        }
        return; // consume input while popup is open
    }
    if (event->type == INPUT_TYPE_TOUCH) {
        lv_indev_data_t *d = &event->data.touch_data;
        if (d->state == LV_INDEV_STATE_PR) return; // handle only on release
        int x = d->point.x;
        int y = d->point.y;
        // check buttons
        for (int i = 0; i < num_items; ++i) {
            lv_obj_t *btn = lv_obj_get_child(menu_container, i);
            if (!btn) continue;
            lv_area_t a;
            lv_obj_get_coords(btn, &a);
            if (x >= a.x1 && x <= a.x2 && y >= a.y1 && y <= a.y2) {
                selected_index = i;
                highlight_selected();
                execute_selected();
                return;
            }
        }
        // touch outside -> back
        display_manager_switch_view(&main_menu_view);
    } else if (event->type == INPUT_TYPE_JOYSTICK) {
        int btn = event->data.joystick_index;
        if (btn == 2) { // up
            selected_index = (selected_index - 1 + num_items) % num_items;
            highlight_selected();
        } else if (btn == 4) { // down
            selected_index = (selected_index + 1) % num_items;
            highlight_selected();
        } else if (btn == 1) { // select
            lv_obj_t *selected_obj = lv_obj_get_child(menu_container, selected_index);
            if (selected_obj) {
                const char *opt = (const char *)lv_obj_get_user_data(selected_obj);
                if (opt) {
                    lv_event_t e; memset(&e, 0, sizeof(e)); e.user_data = (void *)opt;
                    nfc_option_event_cb(&e);
                } else {
                    execute_selected();
                }
            } else {
                execute_selected();
            }
        } else if (btn == 0) { // back
            display_manager_switch_view(&main_menu_view);
        }
    } else if (event->type == INPUT_TYPE_ENCODER) {
        if (event->data.encoder.button) {
            lv_obj_t *sel = lv_obj_get_child(menu_container, selected_index);
            if (sel) {
                const char *opt = (const char *)lv_obj_get_user_data(sel);
                if (opt && strcmp(opt, "__BACK_OPTION__") == 0) back_event_cb(NULL);
                else if (opt) { lv_event_t e; memset(&e, 0, sizeof(e)); e.user_data = (void *)opt; nfc_option_event_cb(&e); }
                else execute_selected();
            } else execute_selected();
        } else {
            if (event->data.encoder.direction > 0)
                selected_index = (selected_index + 1) % num_items;
            else
                selected_index = (selected_index - 1 + num_items) % num_items;
            highlight_selected();
        }
    } else if (event->type == INPUT_TYPE_KEYBOARD) {
        int kv = event->data.key_value;
        if (kv == 13) { // Enter
            lv_obj_t *selected_obj = lv_obj_get_child(menu_container, selected_index);
            if (selected_obj) {
                const char *opt = (const char *)lv_obj_get_user_data(selected_obj);
                if (opt) { lv_event_t e; memset(&e, 0, sizeof(e)); e.user_data = (void *)opt; nfc_option_event_cb(&e); }
                else execute_selected();
            } else execute_selected();
        } else if (kv == 44 || kv == ',') { // left/up
            selected_index = (selected_index - 1 + num_items) % num_items;
            highlight_selected();
        } else if (kv == 47 || kv == '/') { // right/down
            selected_index = (selected_index + 1) % num_items;
            highlight_selected();
        } else if (kv == 29 || kv == '`') { // Esc
            display_manager_switch_view(&main_menu_view);
        }
#ifdef CONFIG_USE_ENCODER
    } else if (event->type == INPUT_TYPE_EXIT_BUTTON) {
        display_manager_switch_view(&main_menu_view);
#endif
    }
}

static void nfc_option_event_cb(lv_event_t *e) {
    // user_data is const char* option
    const char *opt = (const char *)lv_event_get_user_data(e);
    if (!opt) return;
    if (strcmp(opt, "__BACK_OPTION__") == 0) {
        back_event_cb(NULL);
        return;
    }

    if (strcmp(opt, "Scan") == 0) {
        create_nfc_scan_popup();
        return;
    }

    // no bottom status label; actions can be wired here if needed
}

// touchscreen scroll callbacks
static void scroll_nfc_up(lv_event_t *e) {
    if (!menu_container) return;
    lv_coord_t scroll_amt = lv_obj_get_height(menu_container) / 2;
    lv_obj_scroll_by_bounded(menu_container, 0, scroll_amt, LV_ANIM_OFF);
}
static void scroll_nfc_down(lv_event_t *e) {
    if (!menu_container) return;
    lv_coord_t scroll_amt = lv_obj_get_height(menu_container) / 2;
    lv_obj_scroll_by_bounded(menu_container, 0, -scroll_amt, LV_ANIM_OFF);
}
static void back_event_cb(lv_event_t *e) {
    display_manager_switch_view(&main_menu_view);
}

static lv_style_t* get_zebra_style(int index) {
    if (settings_get_zebra_menus_enabled(&G_Settings)) return (index % 2 == 0) ? &style_menu_item : &style_menu_item_alt;
    return &style_menu_item;
}

static void cleanup_nfc_scan_popup(void *obj) {
    if (nfc_scan_popup) {
        lv_obj_del(nfc_scan_popup);
        nfc_scan_popup = NULL;
        nfc_scan_cancel_btn = NULL;
        nfc_uid_label = NULL;
        nfc_type_label = NULL;
    }
}

static void nfc_scan_cancel_cb(lv_event_t *e) {
    cleanup_nfc_scan_popup(NULL);
    display_manager_switch_view(&nfc_view);
}

static void create_nfc_scan_popup(void) {
    if (nfc_scan_popup && lv_obj_is_valid(nfc_scan_popup)) {
        cleanup_nfc_scan_popup(NULL);
    }
    if (!root || !lv_obj_is_valid(root)) return;
    nfc_scan_popup = lv_obj_create(root);
    // scale to screen, leave margin for status bar and edges
    int popup_w = LV_HOR_RES - 30;
    int popup_h = (LV_VER_RES <= 240) ? 140 : 160;
    lv_obj_set_size(nfc_scan_popup, popup_w, popup_h);
    lv_obj_align(nfc_scan_popup, LV_ALIGN_TOP_MID, 0, 24); // below status bar
    lv_obj_set_style_bg_color(nfc_scan_popup, lv_color_hex(0x2E2E2E), 0);
    lv_obj_set_style_border_color(nfc_scan_popup, lv_color_hex(0x555555), 0);
    lv_obj_set_style_border_width(nfc_scan_popup, 2, 0);
    lv_obj_set_style_radius(nfc_scan_popup, 10, 0);
    lv_obj_clear_flag(nfc_scan_popup, LV_OBJ_FLAG_SCROLLABLE);

    // Title
    lv_obj_t *title = lv_label_create(nfc_scan_popup);
    lv_label_set_text(title, "Scanning NFC...");
    const lv_font_t *title_font = (LV_VER_RES <= 240) ? &lv_font_montserrat_14 : &lv_font_montserrat_16;
    lv_obj_set_style_text_font(title, title_font, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 2);

    // Placeholder fields (UID / Type)
    nfc_uid_label = lv_label_create(nfc_scan_popup);
    lv_label_set_text(nfc_uid_label, "UID: -- -- -- -- -- -- -- --");
    lv_obj_set_style_text_color(nfc_uid_label, lv_color_hex(0xCCCCCC), 0);
    const lv_font_t *body_font = (LV_VER_RES <= 240) ? &lv_font_montserrat_12 : &lv_font_montserrat_14;
    lv_obj_set_style_text_font(nfc_uid_label, body_font, 0);
    lv_obj_align(nfc_uid_label, LV_ALIGN_TOP_MID, 0, 22);

    nfc_type_label = lv_label_create(nfc_scan_popup);
    lv_label_set_text(nfc_type_label, "Type: --");
    lv_obj_set_style_text_color(nfc_type_label, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_text_font(nfc_type_label, body_font, 0);
    lv_obj_align(nfc_type_label, LV_ALIGN_TOP_MID, 0, 40);

    // Cancel button
    nfc_scan_cancel_btn = lv_btn_create(nfc_scan_popup);
    int btn_w = 90, btn_h = 34;
    if (LV_VER_RES <= 240) { btn_w = 80; btn_h = 30; }
    lv_obj_set_size(nfc_scan_cancel_btn, btn_w, btn_h);
    lv_obj_align(nfc_scan_cancel_btn, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_set_style_bg_color(nfc_scan_cancel_btn, lv_color_hex(0x444444), 0);
    lv_obj_set_style_border_color(nfc_scan_cancel_btn, lv_color_hex(0x666666), 0);
    lv_obj_set_style_border_width(nfc_scan_cancel_btn, 1, 0);
    lv_obj_t *cancel_label = lv_label_create(nfc_scan_cancel_btn);
    lv_label_set_text(cancel_label, "Cancel");
    lv_obj_set_style_text_font(cancel_label, body_font, 0);
    lv_obj_center(cancel_label);
    lv_obj_add_event_cb(nfc_scan_cancel_btn, nfc_scan_cancel_cb, LV_EVENT_CLICKED, NULL);
    nfc_popup_selected = 0;
    update_nfc_popup_selection();
}

static void update_nfc_popup_selection(void) {
    if (!nfc_scan_cancel_btn) return;
    // Only one selectable control: highlight it (white bg, black text) like IR popups
    if (nfc_popup_selected == 0) {
        lv_obj_set_style_bg_color(nfc_scan_cancel_btn, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(nfc_scan_cancel_btn, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_t *lbl = lv_obj_get_child(nfc_scan_cancel_btn, 0);
        if (lbl) lv_obj_set_style_text_color(lbl, lv_color_hex(0x000000), 0);
    } else {
        lv_obj_set_style_bg_color(nfc_scan_cancel_btn, lv_color_hex(0x444444), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(nfc_scan_cancel_btn, lv_color_hex(0x666666), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_t *lbl = lv_obj_get_child(nfc_scan_cancel_btn, 0);
        if (lbl) lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
    }
}

static void nfc_view_create(void) {
    lv_obj_clear_flag(lv_scr_act(), LV_OBJ_FLAG_SCROLLABLE);
    root = lv_obj_create(lv_scr_act());
    nfc_view.root = root;
    lv_obj_set_size(root, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(root, lv_color_hex(0x121212), 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_align(root, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_scrollbar_mode(root, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    init_styles();

    int screen_width = LV_HOR_RES;
    int screen_height = LV_VER_RES;
    const int STATUS_BAR_HEIGHT = 20;
    bool is_small_screen = (screen_width <= 240 || screen_height <= 240);
#ifdef CONFIG_USE_TOUCHSCREEN
    const int BUTTON_AREA_HEIGHT = SCROLL_BTN_SIZE + SCROLL_BTN_PADDING * 2;
#else
    const int BUTTON_AREA_HEIGHT = 0;
#endif
    int container_height = screen_height - STATUS_BAR_HEIGHT - BUTTON_AREA_HEIGHT;

    is_small_screen_global = is_small_screen;
    button_height_global = is_small_screen ? 40 : 55;

    menu_container = lv_list_create(root);
    lv_obj_set_style_radius(menu_container, 0, LV_PART_MAIN);
    lv_obj_set_size(menu_container, screen_width, container_height);
    lv_obj_align(menu_container, LV_ALIGN_TOP_MID, 0, STATUS_BAR_HEIGHT);
    lv_obj_set_style_bg_color(menu_container, lv_color_hex(0x121212), 0);
    lv_obj_set_style_pad_all(menu_container, 0, 0);
    lv_obj_set_style_pad_top(menu_container, 0, 0);
    lv_obj_set_style_pad_bottom(menu_container, 0, 0);
    lv_obj_set_style_border_width(menu_container, 0, 0);
    lv_obj_set_scrollbar_mode(menu_container, LV_SCROLLBAR_MODE_OFF);

    // Add Scan button
    scan_btn = lv_list_add_btn(menu_container, NULL, "Scan");
    lv_obj_set_height(scan_btn, button_height_global);
    lv_obj_add_style(scan_btn, get_zebra_style(0), 0);
    lv_obj_t *slabel = lv_obj_get_child(scan_btn, 0);
    if (slabel) {
        lv_obj_set_style_text_font(slabel, get_menu_font(), 0);
        vertically_center_label(slabel, scan_btn);
        lv_obj_add_style(slabel, &style_menu_label, 0);
    }
    lv_obj_set_user_data(scan_btn, (void *)"Scan");
    lv_obj_add_event_cb(scan_btn, nfc_option_event_cb, LV_EVENT_CLICKED, (void *)"Scan");

    // Add Emulate button
    emulate_btn = lv_list_add_btn(menu_container, NULL, "Emulate NFC Tag");
    lv_obj_set_height(emulate_btn, button_height_global);
    lv_obj_add_style(emulate_btn, get_zebra_style(1), 0);
    lv_obj_t *elabel = lv_obj_get_child(emulate_btn, 0);
    if (elabel) {
        lv_obj_set_style_text_font(elabel, get_menu_font(), 0);
        vertically_center_label(elabel, emulate_btn);
        lv_obj_add_style(elabel, &style_menu_label, 0);
    }
    lv_obj_set_user_data(emulate_btn, (void *)"Emulate NFC Tag");
    lv_obj_add_event_cb(emulate_btn, nfc_option_event_cb, LV_EVENT_CLICKED, (void *)"Emulate NFC Tag");

    num_items = 2;

#ifdef CONFIG_USE_ENCODER
    // Add Back option row for encoder users, mirroring options_screen behavior
    lv_obj_t *back_row = lv_list_add_btn(menu_container, NULL, LV_SYMBOL_LEFT " Back");
    if (back_row) {
        lv_obj_set_height(back_row, button_height_global);
        lv_obj_add_style(back_row, get_zebra_style(2), 0);
        lv_obj_t *blabel = lv_obj_get_child(back_row, 0);
        if (blabel) {
            lv_obj_set_style_text_font(blabel, get_menu_font(), 0);
            vertically_center_label(blabel, back_row);
            lv_obj_add_style(blabel, &style_menu_label, 0);
        }
        lv_obj_set_user_data(back_row, (void *)"__BACK_OPTION__");
        lv_obj_add_event_cb(back_row, nfc_option_event_cb, LV_EVENT_CLICKED, (void *)"__BACK_OPTION__");
        num_items++;
    }
#endif

    // add touchscreen nav buttons + back button (same style as options_screen)
#ifdef CONFIG_USE_TOUCHSCREEN
    scroll_up_btn = lv_btn_create(root);
    lv_obj_set_size(scroll_up_btn, SCROLL_BTN_SIZE, SCROLL_BTN_SIZE);
    lv_obj_align(scroll_up_btn, LV_ALIGN_BOTTOM_LEFT, SCROLL_BTN_PADDING, -SCROLL_BTN_PADDING);
    lv_obj_set_style_bg_color(scroll_up_btn, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_radius(scroll_up_btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_border_width(scroll_up_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(scroll_up_btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(scroll_up_btn, scroll_nfc_up, LV_EVENT_CLICKED, NULL);
    lv_obj_t *up_label = lv_label_create(scroll_up_btn);
    lv_label_set_text(up_label, LV_SYMBOL_UP);
    lv_obj_center(up_label);

    scroll_down_btn = lv_btn_create(root);
    lv_obj_set_size(scroll_down_btn, SCROLL_BTN_SIZE, SCROLL_BTN_SIZE);
    lv_obj_align(scroll_down_btn, LV_ALIGN_BOTTOM_RIGHT, -SCROLL_BTN_PADDING, -SCROLL_BTN_PADDING);
    lv_obj_set_style_bg_color(scroll_down_btn, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_radius(scroll_down_btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_border_width(scroll_down_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(scroll_down_btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(scroll_down_btn, scroll_nfc_down, LV_EVENT_CLICKED, NULL);
    lv_obj_t *down_label = lv_label_create(scroll_down_btn);
    lv_label_set_text(down_label, LV_SYMBOL_DOWN);
    lv_obj_center(down_label);

    back_btn = lv_btn_create(root);
    lv_obj_set_size(back_btn, SCROLL_BTN_SIZE + 20, SCROLL_BTN_SIZE);
    lv_obj_align(back_btn, LV_ALIGN_BOTTOM_MID, 0, -SCROLL_BTN_PADDING);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0x555555), LV_PART_MAIN);
    lv_obj_set_style_radius(back_btn, 5, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(back_btn, 10, LV_PART_MAIN);
    lv_obj_set_style_border_width(back_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(back_btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(back_btn, back_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_label = lv_label_create(back_btn);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT " Back");
    lv_obj_center(back_label);
#endif

    highlight_selected();

    display_manager_add_status_bar("NFC");
}

static void nfc_view_destroy(void) {
    if (root) {
        lv_obj_del(root);
        root = NULL;
    }
    nfc_view.root = NULL;
    menu_container = NULL;
    scan_btn = NULL;
    emulate_btn = NULL;
    scroll_up_btn = NULL;
    scroll_down_btn = NULL;
    back_btn = NULL;
    nfc_scan_popup = NULL;
    nfc_scan_cancel_btn = NULL;
    nfc_uid_label = NULL;
    nfc_type_label = NULL;
}

static void get_nfc_callback(void **cb) {
    if (cb) *cb = nfc_view_input_cb;
}

View nfc_view = {
    .root = NULL,
    .create = nfc_view_create,
    .destroy = nfc_view_destroy,
    .input_callback = nfc_view_input_cb,
    .name = "NFC",
    .get_hardwareinput_callback = get_nfc_callback
};
