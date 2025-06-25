#include "managers/views/keyboard_screen.h"
#include "core/serial_manager.h"
#include "managers/views/options_screen.h"
#include "managers/views/terminal_screen.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <string.h>
#include <ctype.h>

static const char *TAG = "keyboard_screen";

static lv_obj_t *root = NULL;
static lv_obj_t *input_label = NULL;
static char input_buffer[128] = {0};
static int input_len = 0;
static KeyboardSubmitCallback submit_callback = NULL;

static bool is_caps = true;

static const char *keys[][10] = {
    {"Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P"},
    {"A", "S", "D", "F", "G", "H", "J", "K", "L"},
    {"SHIFT", "Z", "X", "C", "V", "B", "N", "M"},
    {",", ".", "\"", " ", "DEL"},
    {"Exit", "Done"}
};
static const int row_lengths[] = {10, 9, 8, 5, 2};
static const int num_rows = 5;

static void submit_text();
static void add_char_to_buffer(char c);
static void remove_char_from_buffer();
static void update_input_label();
static void update_key_labels();

static void submit_text() {
    if (input_len > 0) {
        if (submit_callback) {
            submit_callback(input_buffer);
        } else {
            display_manager_switch_view(&terminal_view);
            vTaskDelay(pdMS_TO_TICKS(10));
            simulateCommand(input_buffer);
        }
        memset(input_buffer, 0, sizeof(input_buffer));
        input_len = 0;
        update_input_label();
    }
}

static void add_char_to_buffer(char c) {
    if (input_len < sizeof(input_buffer) - 1) {
        input_buffer[input_len++] = c;
        input_buffer[input_len] = '\0';
        update_input_label();
    }
}

static void remove_char_from_buffer() {
    if (input_len > 0) {
        input_buffer[--input_len] = '\0';
        update_input_label();
    }
}

static void update_input_label() {
    if (input_label) {
        lv_label_set_text(input_label, input_buffer);
    }
}

static void update_key_labels() {
#ifdef CONFIG_USE_TOUCHSCREEN
    if (!root) return;
    
    int key_index = 0;
    uint32_t child_count = lv_obj_get_child_cnt(root);
    
    for (int r = 0; r < num_rows; r++) {
        for (int c = 0; c < row_lengths[r]; c++) {
            int child_idx = 1 + key_index;
            if (child_idx < child_count) {
                lv_obj_t *key_btn = lv_obj_get_child(root, child_idx);
                if (key_btn) {
                    lv_obj_t *key_label = lv_obj_get_child(key_btn, 0);
                    if (key_label) {
                        const char* key_text = keys[r][c];
                        if (strlen(key_text) == 1) {
                            char new_text[2];
                            new_text[0] = is_caps ? toupper(key_text[0]) : tolower(key_text[0]);
                            new_text[1] = '\0';
                            lv_label_set_text(key_label, new_text);
                            lv_obj_set_style_text_color(key_label, lv_color_hex(0xFFFFFF), 0);
                        }
                    }
                }
            }
            key_index++;
        }
    }
#endif
}

static void keyboard_create() {
    is_caps = true;
    input_len = 0;
    memset(input_buffer, 0, sizeof(input_buffer));

    int screen_width = LV_HOR_RES;
    int screen_height = LV_VER_RES;
    int status_bar_height = 20;

    root = lv_obj_create(lv_scr_act());
    keyboard_view.root = root;
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, screen_width, screen_height);
    lv_obj_set_style_bg_color(root, lv_color_hex(0x121212), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    int padding = 5;
    int display_height = 40;
    input_label = lv_label_create(root);
    lv_obj_set_size(input_label, screen_width - 2 * padding, display_height - 2 * padding);
    lv_obj_set_style_bg_color(input_label, lv_color_hex(0x1E1E1E), 0);
    lv_obj_set_style_bg_opa(input_label, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(input_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_pad_all(input_label, padding, 0);
    lv_obj_set_style_radius(input_label, 5, 0);
    lv_obj_set_pos(input_label, padding, status_bar_height + padding);
    lv_label_set_long_mode(input_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    update_input_label();

#ifdef CONFIG_USE_TOUCHSCREEN
    int keys_start_y = status_bar_height + display_height + padding * 2;
    int keys_area_height = screen_height - keys_start_y;
    int key_height = (keys_area_height / num_rows) - 4;
    int key_y = keys_start_y;

    for (int r = 0; r < num_rows; r++) {
        int total_key_width = screen_width - (padding * 2);
        int key_width = total_key_width / row_lengths[r];
        int key_x = padding;
        
        for (int c = 0; c < row_lengths[r]; c++) {
            lv_obj_t *key_btn = lv_btn_create(root);
            lv_obj_remove_style_all(key_btn);
            lv_obj_set_size(key_btn, key_width - 2, key_height);
            lv_obj_set_pos(key_btn, key_x, key_y);
            
            lv_obj_set_style_bg_color(key_btn, lv_color_hex(0x7B1FA2), 0);
            lv_obj_set_style_bg_opa(key_btn, LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(key_btn, lv_color_hex(0x444444), 0);
            lv_obj_set_style_border_width(key_btn, 1, 0);
            lv_obj_set_style_radius(key_btn, 3, 0);

            lv_obj_t *key_label = lv_label_create(key_btn);
            lv_label_set_text(key_label, keys[r][c]);
            lv_obj_set_style_text_color(key_label, lv_color_hex(0xFFFFFF), 0);
            lv_obj_set_style_text_font(key_label, &lv_font_montserrat_14, 0);
            lv_obj_center(key_label);
            
            key_x += key_width;
        }
        key_y += key_height + 2;
    }
#endif
    
    display_manager_add_status_bar("Keyboard");
}

static void keyboard_destroy() {
    if (keyboard_view.root) {
        lv_obj_del(keyboard_view.root);
        keyboard_view.root = NULL;
        root = NULL;
        input_label = NULL;
        submit_callback = NULL;
        input_len = 0;
        input_buffer[0] = '\0';
    }
}

static void handle_hardware_button_press_keyboard(InputEvent *event) {
    if (event->type == INPUT_TYPE_TOUCH && event->data.touch_data.state == LV_INDEV_STATE_PR) {
        int touch_x = event->data.touch_data.point.x;
        int touch_y = event->data.touch_data.point.y;
        
        int screen_width = LV_HOR_RES;
        int screen_height = LV_VER_RES;
        int status_bar_height = 20;
        
        int display_height = 40;
        int padding = 5;
        int keys_area_y = status_bar_height + display_height + padding;

        if (touch_y < keys_area_y) return;

        int key_height = (screen_height - keys_area_y - 15) / num_rows;
        
        int row = (touch_y - keys_area_y) / key_height;

        if (row >= 0 && row < num_rows) {
            int key_width = screen_width / row_lengths[row];
            int col = touch_x / key_width;

            if (col >= 0 && col < row_lengths[row]) {
                const char* key = keys[row][col];
                if (strcmp(key, "SHIFT") == 0) {
                    is_caps = !is_caps;
                    update_key_labels();
                } else if (strcmp(key, "Exit") == 0) {
                    display_manager_switch_view(&options_menu_view);
                } else if (strcmp(key, "Done") == 0) {
                    submit_text();
                } else if (strcmp(key, "DEL") == 0) {
                    remove_char_from_buffer();
                } else if (strcmp(key, " ") == 0) {
                    add_char_to_buffer(' ');
                } else if (strlen(key) == 1) {
                    add_char_to_buffer(is_caps ? toupper(key[0]) : tolower(key[0]));
                }
            }
        }
    } else if (event->type == INPUT_TYPE_KEYBOARD) {
        char c = (char)event->data.key_value;
        if (c == '`') {
            display_manager_switch_view(&options_menu_view);
        } else if (c == '\n' || c == '\r' || c == '=') {
            submit_text();
        } else if (c == '\b' || c == '*') {
            remove_char_from_buffer();
        } else if (c >= ' ' && c <= '~') {
            add_char_to_buffer(c);
        }
    }
}

static void get_keyboard_callback(void **callback) {
    *callback = keyboard_view.input_callback;
}

void keyboard_view_set_submit_callback(KeyboardSubmitCallback cb){
    submit_callback = cb;
}

void keyboard_view_set_placeholder(const char *text){
    if(input_label){
        lv_label_set_text(input_label, text);
    }
    memset(input_buffer, 0, sizeof(input_buffer));
    input_len = 0;
}

View keyboard_view = {
    .root = NULL,
    .create = keyboard_create,
    .destroy = keyboard_destroy,
    .input_callback = handle_hardware_button_press_keyboard,
    .name = "Keyboard Screen",
    .get_hardwareinput_callback = get_keyboard_callback
}; 