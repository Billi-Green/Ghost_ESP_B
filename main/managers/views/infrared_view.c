#include "managers/views/infrared_view.h"
#include "managers/display_manager.h"
#include "managers/views/main_menu_screen.h"
#include <lvgl/lvgl.h>
#include <dirent.h>
#include <string.h>
#include "managers/infrared_manager.h"
#include "managers/rgb_manager.h"
#include "sdkconfig.h"
#include "esp_log.h"

// Infrared view UI
static lv_obj_t *root = NULL;
static lv_obj_t *list = NULL;
static int selected_ir_index = 0;
static int num_ir_items = 0;
static char **ir_file_paths = NULL;
static size_t ir_file_count = 0;
static infrared_signal_t *signals = NULL;
static size_t signal_count = 0;
static bool showing_commands = false;
static char current_dir[256] = "/mnt/ghostesp";
static bool has_remotes_option = false;

// touchscreen controls
#ifdef CONFIG_USE_TOUCHSCREEN
#define IR_SCROLL_BTN_SIZE 40
#define IR_SCROLL_BTN_PADDING 5
static lv_obj_t *ir_scroll_up_btn = NULL;
static lv_obj_t *ir_scroll_down_btn = NULL;
static lv_obj_t *ir_back_btn = NULL;
// scroll callbacks
static void file_scroll_up_cb(lv_event_t *e) { ir_select_item(selected_ir_index - 1); }
static void file_scroll_down_cb(lv_event_t *e) { ir_select_item(selected_ir_index + 1); }
#endif

// forward declarations
static void ir_select_item(int index);
static void back_event_cb(lv_event_t *e);
static void file_event_open(int idx);
static void command_event_execute(int idx);
static void file_event_cb(lv_event_t *e);
static void command_event_cb(lv_event_t *e);
static void remotes_event_cb(lv_event_t *e);

static const char *TAG = "infrared_view";

static void back_event_cb(lv_event_t *e) {
    display_manager_switch_view(&main_menu_view);
}

void infrared_view_create(void) {
    root = lv_obj_create(lv_scr_act());
    lv_obj_set_style_pad_all(root, 0, 0);
    infrared_view.root = root;
    lv_obj_set_size(root, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(root, lv_color_hex(0x121212), 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_width(root, 0, LV_PART_MAIN);

    display_manager_add_status_bar("Infrared");

    const int STATUS_BAR_HEIGHT = 20;
    int list_h = LV_VER_RES - STATUS_BAR_HEIGHT;
    list = lv_list_create(root);
    lv_obj_set_style_pad_all(list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_left(list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_top(list, 0, LV_PART_MAIN);
    lv_obj_set_size(list, LV_HOR_RES, list_h);
    lv_obj_align(list, LV_ALIGN_TOP_LEFT, 0, STATUS_BAR_HEIGHT);
    lv_obj_set_style_bg_color(list, lv_color_hex(0x121212), 0);
    lv_obj_set_style_border_width(list, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(list, 0, LV_PART_MAIN);

    // add remotes option
    has_remotes_option = true;
    lv_obj_t *remotes_btn = lv_list_add_btn(list, NULL, "Remotes");
    lv_obj_set_width(remotes_btn, LV_HOR_RES);
    lv_obj_set_style_bg_color(remotes_btn, lv_color_hex(0x1E1E1E), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(remotes_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(remotes_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(remotes_btn, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_t *rem_label = lv_obj_get_child(remotes_btn, 0);
    if (rem_label) {
        lv_obj_set_style_text_font(rem_label, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(rem_label, lv_color_hex(0xFFFFFF), 0);
    }
    lv_obj_add_event_cb(remotes_btn, remotes_event_cb, LV_EVENT_CLICKED, NULL);

    // initialize selection
    selected_ir_index = 0;
    num_ir_items = ir_file_count + (has_remotes_option ? 1 : 0);
    if (num_ir_items > 0) ir_select_item(0);

    // Back button
    // touchscreen-only controls
    #ifdef CONFIG_USE_TOUCHSCREEN
    ir_scroll_up_btn = lv_btn_create(root);
    lv_obj_set_size(ir_scroll_up_btn, IR_SCROLL_BTN_SIZE, IR_SCROLL_BTN_SIZE);
    lv_obj_align(ir_scroll_up_btn, LV_ALIGN_BOTTOM_LEFT, IR_SCROLL_BTN_PADDING, -IR_SCROLL_BTN_PADDING);
    lv_obj_set_style_bg_color(ir_scroll_up_btn, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_radius(ir_scroll_up_btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_add_event_cb(ir_scroll_up_btn, file_scroll_up_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *up_label = lv_label_create(ir_scroll_up_btn);
    lv_label_set_text(up_label, LV_SYMBOL_UP);
    lv_obj_center(up_label);

    ir_scroll_down_btn = lv_btn_create(root);
    lv_obj_set_size(ir_scroll_down_btn, IR_SCROLL_BTN_SIZE, IR_SCROLL_BTN_SIZE);
    lv_obj_align(ir_scroll_down_btn, LV_ALIGN_BOTTOM_RIGHT, -IR_SCROLL_BTN_PADDING, -IR_SCROLL_BTN_PADDING);
    lv_obj_set_style_bg_color(ir_scroll_down_btn, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_radius(ir_scroll_down_btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_add_event_cb(ir_scroll_down_btn, file_scroll_down_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *down_label = lv_label_create(ir_scroll_down_btn);
    lv_label_set_text(down_label, LV_SYMBOL_DOWN);
    lv_obj_center(down_label);

    ir_back_btn = lv_btn_create(root);
    lv_obj_set_size(ir_back_btn, IR_SCROLL_BTN_SIZE + 20, IR_SCROLL_BTN_SIZE);
    lv_obj_align(ir_back_btn, LV_ALIGN_BOTTOM_MID, 0, -IR_SCROLL_BTN_PADDING);
    lv_obj_set_style_bg_color(ir_back_btn, lv_color_hex(0x555555), LV_PART_MAIN);
    lv_obj_set_style_radius(ir_back_btn, 5, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(ir_back_btn, 10, LV_PART_MAIN);
    lv_obj_add_event_cb(ir_back_btn, back_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_label = lv_label_create(ir_back_btn);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT " Back");
    lv_obj_center(back_label);
    #endif
}

void infrared_view_destroy(void) {
    if(root) {
        if(signals) {
            infrared_manager_free_list(signals, signal_count);
            signals = NULL;
            signal_count = 0;
        }
        if(ir_file_paths) {
            for(size_t i = 0; i < ir_file_count; i++) {
                free(ir_file_paths[i]);
            }
            free(ir_file_paths);
            ir_file_paths = NULL;
            ir_file_count = 0;
        }
        showing_commands = false;
        lv_obj_del(root);
        root = NULL;
        list = NULL;
        infrared_view.root = NULL;
        selected_ir_index = 0;
        num_ir_items = 0;
    }
}

static void ir_select_item(int index) {
    if(num_ir_items == 0) return;
    if(index < 0) index = num_ir_items - 1;
    if(index >= num_ir_items) index = 0;
    // clear previous
    lv_obj_t *prev = lv_obj_get_child(list, selected_ir_index);
    if(prev) {
        lv_obj_set_style_bg_color(prev, lv_color_hex(0x1E1E1E), LV_PART_MAIN);
    }
    selected_ir_index = index;
    lv_obj_t *cur = lv_obj_get_child(list, selected_ir_index);
    if(cur) {
        lv_obj_set_style_bg_color(cur, lv_color_hex(0x555555), LV_PART_MAIN);
        lv_obj_scroll_to_view(cur, LV_ANIM_OFF);
    }
}

void infrared_view_input_cb(InputEvent *event) {
    if(event->type == INPUT_TYPE_JOYSTICK) {
        uint8_t idx = event->data.joystick_index;
        if(idx == 0) { // left => back
            ESP_LOGI(TAG, "joystick left pressed, going back");
            back_event_cb(NULL);
        } else if(idx == 2) { // up
            ESP_LOGI(TAG, "joystick up pressed, selecting previous item");
            ir_select_item(selected_ir_index - 1);
        } else if(idx == 4) { // down
            ESP_LOGI(TAG, "joystick down pressed, selecting next item");
            ir_select_item(selected_ir_index + 1);
        } else if(idx == 1) { // enter
            if (!showing_commands) {
                if (has_remotes_option && selected_ir_index == 0) {
                    ESP_LOGI(TAG, "joystick enter pressed on Remotes, opening remotes directory");
                    remotes_event_cb(NULL);
                } else {
                    int file_idx = selected_ir_index - (has_remotes_option ? 1 : 0);
                    ESP_LOGI(TAG, "joystick enter pressed, opening selected file at index %d", file_idx);
                    file_event_open(file_idx);
                }
            } else {
                ESP_LOGI(TAG, "joystick enter pressed, executing selected command at index %d", selected_ir_index);
                command_event_execute(selected_ir_index);
            }
        }
    }
}

// open selected IR file and list commands
static void file_event_open(int idx) {
    if (idx < 0 || idx >= ir_file_count) return;
    char path[256];
    size_t base_len = strlen(current_dir);
    if (base_len >= sizeof(path) - 1) base_len = sizeof(path) - 1;
    memcpy(path, current_dir, base_len);
    path[base_len] = '\0';
    if (base_len + 1 < sizeof(path)) {
        strncat(path, "/", sizeof(path) - strlen(path) - 1);
    }
    strncat(path, ir_file_paths[idx], sizeof(path) - strlen(path) - 1);

    ESP_LOGI(TAG, "opening IR file: %s", path);

    if (signals) {
        infrared_manager_free_list(signals, signal_count);
        signals = NULL;
        signal_count = 0;
    }
    if (!infrared_manager_read_list(path, &signals, &signal_count)) {
        ESP_LOGE(TAG, "failed to read IR list from file: %s", path);
        return;
    }
    lv_obj_clean(list);
    showing_commands = true;
    num_ir_items = signal_count;
    selected_ir_index = 0;

    ESP_LOGI(TAG, "listing %zu commands for %s", signal_count, ir_file_paths[idx]);

    for (size_t i = 0; i < signal_count; i++) {
        const char *cmd_name = signals[i].name;
        lv_obj_t *btn = lv_list_add_btn(list, NULL, cmd_name);
        lv_obj_set_width(btn, LV_HOR_RES);
        // style button
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x1E1E1E), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(btn, 0, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_t *label = lv_obj_get_child(btn, 0);
        if (label) {
            lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
        }
        lv_obj_add_event_cb(btn, command_event_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
    }
    ir_select_item(0);
}

// execute selected IR command
static void command_event_execute(int idx) {
    if (idx < 0 || idx >= signal_count) return;
    ESP_LOGI(TAG, "transmitting command: %s", signals[idx].name);
    infrared_manager_transmit(&signals[idx]);
}

// LVGL event wrappers
static void file_event_cb(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    file_event_open(idx);
}

static void command_event_cb(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    command_event_execute(idx);
}

static void remotes_event_cb(lv_event_t *e) {
    has_remotes_option = false;
    strcpy(current_dir, "/mnt/ghostesp/infrared/remotes");
    if (signals) {
        infrared_manager_free_list(signals, signal_count);
        signals = NULL;
        signal_count = 0;
    }
    if (ir_file_paths) {
        for (size_t i = 0; i < ir_file_count; i++) {
            free(ir_file_paths[i]);
        }
        free(ir_file_paths);
        ir_file_paths = NULL;
        ir_file_count = 0;
    }
    showing_commands = false;
    lv_obj_clean(list);
    DIR *d = opendir(current_dir);
    if (d) {
        struct dirent *entry;
        while ((entry = readdir(d)) != NULL) {
            const char *name = entry->d_name;
            if (strstr(name, ".ir") || strstr(name, ".IR")) {
                ir_file_paths = realloc(ir_file_paths, (ir_file_count + 1) * sizeof(*ir_file_paths));
                ir_file_paths[ir_file_count] = strdup(name);
                lv_obj_t *btn = lv_list_add_btn(list, NULL, name);
                lv_obj_set_width(btn, LV_HOR_RES);
                lv_obj_set_style_bg_color(btn, lv_color_hex(0x1E1E1E), LV_PART_MAIN | LV_STATE_DEFAULT);
                lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
                lv_obj_set_style_radius(btn, 0, LV_PART_MAIN);
                lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
                lv_obj_t *label = lv_obj_get_child(btn, 0);
                if (label) {
                    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
                    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
                }
                lv_obj_add_event_cb(btn, file_event_cb, LV_EVENT_CLICKED, (void*)(intptr_t)ir_file_count);
                ir_file_count++;
            }
        }
        closedir(d);
    }
    selected_ir_index = 0;
    num_ir_items = ir_file_count;
    if (ir_file_count > 0) ir_select_item(0);
}

// provide hardware input callback registration
static void get_infrared_view_callback(void **callback) {
    *callback = infrared_view_input_cb;
}

// Define the view
View infrared_view = {
    .root = NULL,
    .create = infrared_view_create,
    .destroy = infrared_view_destroy,
    .input_callback = infrared_view_input_cb,
    .name = "Infrared",
    .get_hardwareinput_callback = get_infrared_view_callback,
}; 