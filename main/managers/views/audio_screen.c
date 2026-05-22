#include "managers/views/audio_screen.h"

#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
#if defined(CONFIG_IDF_TARGET_ESP32C5) && defined(CONFIG_BUILD_CONFIG_TEMPLATE)

#include "managers/views/app_gallery_screen.h"
#include "managers/views/main_menu_screen.h"
#include "managers/audio_manager.h"
#include "managers/sd_card_manager.h"
#include "managers/display_manager.h"
#include "managers/microphone/mic_visualizer.h"
#include "gui/lvgl_safe.h"
#include "gui/screen_layout.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

static const char *TAG = "AudioApp";

#define MAX_MP3_FILES 32
#define AUDIO_DIR "/mnt/ghostesp/audio"

static lv_obj_t *audio_root = NULL;
static lv_obj_t *file_list = NULL;
static lv_obj_t *status_label = NULL;
static lv_obj_t *file_name_label = NULL;
static lv_obj_t *play_btn = NULL;
static lv_obj_t *stop_btn = NULL;
static bool sd_mounted = false;
static bool sd_display_suspended = false;
static volatile bool audio_playback_active = false;

static mp3_file_info_t mp3_files[MAX_MP3_FILES];
static int mp3_file_count = 0;
static int selected_file_index = -1;
static volatile bool scan_in_progress = false;

static void refresh_file_list(void);
static void update_status_label(void);
static void scan_complete_cb(void *);

static void scan_mp3_files_task(void *arg) {
    (void)arg;
    scan_in_progress = true;

    int count = audio_scan_mp3_files(mp3_files, MAX_MP3_FILES, AUDIO_DIR);
    mp3_file_count = count;
    ESP_LOGI(TAG, "Found %d MP3 files", mp3_file_count);

    scan_in_progress = false;

    // Notify LVGL task to update UI
    lv_async_call(scan_complete_cb, NULL);
    vTaskDelete(NULL);
}

static void scan_complete_cb(void *) {
    if (audio_root == NULL) return;
    refresh_file_list();
    update_status_label();
}

static void start_background_scan(void) {
    StackType_t *stack = heap_caps_malloc(4096 * sizeof(StackType_t), MALLOC_CAP_SPIRAM);
    StaticTask_t *tcb = heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL);
    if (!stack || !tcb) {
        ESP_LOGE(TAG, "Failed to allocate scan task resources");
        heap_caps_free(stack);
        heap_caps_free(tcb);
        scan_in_progress = false;
        return;
    }
    xTaskCreateStatic(scan_mp3_files_task, "audio_scan", 4096, NULL, 2, stack, tcb);
}

static void audio_mount_sd(void) {
    if (sd_mounted) return;
    esp_err_t err = sd_card_mount_for_flush(&sd_display_suspended);
    if (err == ESP_OK) {
        sd_mounted = true;
        ESP_LOGI(TAG, "SD mounted");
    } else {
        ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(err));
    }
}

static void audio_unmount_sd(void) {
    if (!sd_mounted) return;
    sd_card_unmount_after_flush(sd_display_suspended);
    sd_mounted = false;
    sd_display_suspended = false;
    ESP_LOGI(TAG, "SD unmounted");
}

static void file_item_click_cb(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    selected_file_index = idx;
    refresh_file_list();
    if (file_name_label) {
        lv_label_set_text(file_name_label, mp3_files[idx].filename);
    }
}

static void update_status_label(void) {
    if (!status_label) return;

    audio_state_t state = audio_get_state();
    const char *state_text = "";

    switch (state) {
        case AUDIO_STATE_IDLE: state_text = "Idle"; break;
        case AUDIO_STATE_PLAYING: state_text = "Playing"; break;
        case AUDIO_STATE_PAUSED: state_text = "Paused"; break;
        case AUDIO_STATE_ERROR: state_text = "Error"; break;
    }

    const char *current = audio_get_current_file();
    if (current && current[0]) {
        char buf[128];
        snprintf(buf, sizeof(buf), "%s: %s", state_text, current);
        lv_label_set_text(status_label, buf);
    } else {
        lv_label_set_text(status_label, state_text);
    }
}

static void refresh_file_list(void) {
    if (!file_list) return;

    lv_obj_clean(file_list);

    if (mp3_file_count == 0) {
        lv_obj_t *no_files_label = lv_label_create(file_list);
        lv_label_set_text(no_files_label, "No MP3 files found");
        lv_obj_set_style_text_color(no_files_label, lv_color_hex(0x888888), 0);
        lv_obj_set_style_text_font(no_files_label, &lv_font_montserrat_12, 0);
        lv_obj_align(no_files_label, LV_ALIGN_CENTER, 0, 0);
        return;
    }

    for (int i = 0; i < mp3_file_count; i++) {
        lv_obj_t *btn = lv_btn_create(file_list);
        lv_obj_set_width(btn, LV_PCT(100));
        lv_obj_set_height(btn, 36);
        lv_obj_set_style_bg_color(btn, (i == selected_file_index) ? lv_color_hex(0x006600) : lv_color_hex(0x333333), 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(0x555555), 0);
        lv_obj_set_style_radius(btn, 4, 0);

        lv_obj_t *label = lv_label_create(btn);
        lv_label_set_text(label, mp3_files[i].filename);
        lv_obj_set_style_text_color(label, lv_color_white(), 0);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_12, 0);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 10, 0);

        lv_obj_add_event_cb(btn, file_item_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
}

static void play_selected_file(void) {
    if (selected_file_index < 0 || selected_file_index >= mp3_file_count) {
        ESP_LOGW(TAG, "No file selected");
        return;
    }

    esp_err_t ret = audio_play_file(mp3_files[selected_file_index].full_path);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Playing: %s", mp3_files[selected_file_index].filename);
    } else {
        ESP_LOGE(TAG, "Failed to play: %s", mp3_files[selected_file_index].filename);
    }
}

static void stop_btn_cb(lv_event_t *e) {
    (void)e;
    audio_stop();
    update_status_label();
    ESP_LOGI(TAG, "Audio stopped");
}

static void play_btn_cb(lv_event_t *e) {
    (void)e;
    play_selected_file();
}

static void audio_input_handler(InputEvent *event) {
    if (event->type == INPUT_TYPE_JOYSTICK) {
        if (event->data.joystick_index == 1) {
            play_selected_file();
        } else if (event->data.joystick_index == 2) {
            if (selected_file_index > 0) {
                selected_file_index--;
                refresh_file_list();
                if (file_name_label) lv_label_set_text(file_name_label, mp3_files[selected_file_index].filename);
            }
        } else if (event->data.joystick_index == 4) {
            if (selected_file_index < mp3_file_count - 1) {
                selected_file_index++;
                refresh_file_list();
                if (file_name_label) lv_label_set_text(file_name_label, mp3_files[selected_file_index].filename);
            }
        } else if (event->data.joystick_index == 0 || event->data.joystick_index == 3) {
            display_manager_switch_view(&apps_menu_view);
        }
    } else if (event->type == INPUT_TYPE_KEYBOARD) {
        if (event->data.key_value == LV_KEY_ESC || event->data.key_value == '`') {
            display_manager_switch_view(&apps_menu_view);
        } else if (event->data.key_value == LV_KEY_ENTER) {
            play_selected_file();
        } else if (event->data.key_value == LV_KEY_UP) {
            if (selected_file_index > 0) {
                selected_file_index--;
                refresh_file_list();
            }
        } else if (event->data.key_value == LV_KEY_DOWN) {
            if (selected_file_index < mp3_file_count - 1) {
                selected_file_index++;
                refresh_file_list();
            }
        }
    } else if (event->type == INPUT_TYPE_ENCODER) {
        if (event->data.encoder.button) {
            play_selected_file();
        } else {
            if (event->data.encoder.direction > 0 && selected_file_index < mp3_file_count - 1) {
                selected_file_index++;
                refresh_file_list();
            } else if (event->data.encoder.direction < 0 && selected_file_index > 0) {
                selected_file_index--;
                refresh_file_list();
            }
        }
    }
#ifdef CONFIG_USE_ENCODER
    else if (event->type == INPUT_TYPE_EXIT_BUTTON) {
        display_manager_switch_view(&apps_menu_view);
    }
#endif
}

static void get_audio_callback(void **callback) {
    *callback = audio_input_handler;
}

void audio_create(void) {
    ESP_LOGI(TAG, "Creating audio app");

#ifdef CONFIG_HAS_MIC
    mic_pause();
#endif

    if (!audio_manager_is_initialized()) {
        audio_manager_config_t cfg = AUDIO_MANAGER_C5_DEFAULT_CONFIG();
        esp_err_t ret = audio_manager_init(&cfg);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Audio manager init FAILED: %s", esp_err_to_name(ret));
        }
    }

    display_manager_fill_screen(lv_color_black());

    audio_root = gui_screen_create_root(NULL, "Audio", lv_color_black(), LV_OPA_TRANSP);
    audio_view.root = audio_root;

    lv_obj_set_flex_flow(audio_root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(audio_root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(audio_root, 10, 0);
    lv_obj_set_style_pad_row(audio_root, 8, 0);

    file_name_label = lv_label_create(audio_root);
    lv_label_set_text(file_name_label, "Select a file");
    lv_obj_set_style_text_color(file_name_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(file_name_label, &lv_font_montserrat_14, 0);

    status_label = lv_label_create(audio_root);
    lv_label_set_text(status_label, "Idle");
    lv_obj_set_style_text_color(status_label, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_12, 0);

    file_list = lv_obj_create(audio_root);
    lv_obj_set_size(file_list, LV_PCT(100), LV_VER_RES - 160);
    lv_obj_set_style_bg_color(file_list, lv_color_hex(0x222222), 0);
    lv_obj_set_style_border_width(file_list, 1, 0);
    lv_obj_set_style_border_color(file_list, lv_color_hex(0x444444), 0);
    lv_obj_set_style_radius(file_list, 4, 0);
    lv_obj_set_style_pad_all(file_list, 4, 0);
    lv_obj_set_flex_flow(file_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(file_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(file_list, 4, 0);
    lv_obj_set_scrollbar_mode(file_list, LV_SCROLLBAR_MODE_AUTO);

    lv_obj_t *btn_container = lv_obj_create(audio_root);
    lv_obj_set_size(btn_container, LV_PCT(100), 40);
    lv_obj_set_style_bg_opa(btn_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_container, 0, 0);
    lv_obj_set_style_pad_all(btn_container, 0, 0);
    lv_obj_set_flex_flow(btn_container, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_container, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(btn_container, 10, 0);

    play_btn = lv_btn_create(btn_container);
    lv_obj_set_size(play_btn, 80, 36);
    lv_obj_set_style_bg_color(play_btn, lv_color_hex(0x006600), 0);
    lv_obj_add_event_cb(play_btn, play_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *play_label = lv_label_create(play_btn);
    lv_label_set_text(play_label, "Play");
    lv_obj_set_style_text_color(play_label, lv_color_white(), 0);
    lv_obj_center(play_label);

    stop_btn = lv_btn_create(btn_container);
    lv_obj_set_size(stop_btn, 80, 36);
    lv_obj_set_style_bg_color(stop_btn, lv_color_hex(0x660000), 0);
    lv_obj_add_event_cb(stop_btn, stop_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *stop_label = lv_label_create(stop_btn);
    lv_label_set_text(stop_label, "Stop");
    lv_obj_set_style_text_color(stop_label, lv_color_white(), 0);
    lv_obj_center(stop_label);

    // Show scanning message while background task runs
    lv_obj_t *scanning_label = lv_label_create(file_list);
    lv_label_set_text(scanning_label, "Scanning for MP3 files...");
    lv_obj_set_style_text_color(scanning_label, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(scanning_label, &lv_font_montserrat_12, 0);
    lv_obj_align(scanning_label, LV_ALIGN_CENTER, 0, 0);

    scan_in_progress = true;
    start_background_scan();

    ESP_LOGI(TAG, "Audio app created successfully");
}

void audio_destroy(void) {
    ESP_LOGI(TAG, "Destroying audio app");
    audio_stop();

    // Wait for background scan to complete
    int wait_ms = 0;
    while (scan_in_progress && wait_ms < 5000) {
        vTaskDelay(pdMS_TO_TICKS(50));
        wait_ms += 50;
    }

    audio_unmount_sd();
    selected_file_index = -1;
    mp3_file_count = 0;
    lvgl_obj_del_safe(&audio_root);
    audio_view.root = NULL;

#ifdef CONFIG_HAS_MIC
    mic_resume();
#endif
}

View audio_view = {
    .root = NULL,
    .create = audio_create,
    .destroy = audio_destroy,
    .input_callback = audio_input_handler,
    .name = "Audio",
    .get_hardwareinput_callback = get_audio_callback,
};

#else /* CONFIG_IDF_TARGET_ESP32C5 */

#include "managers/views/app_gallery_screen.h"
#include "managers/views/main_menu_screen.h"
#include "gui/lvgl_safe.h"
#include "gui/screen_layout.h"
#include "esp_log.h"

static const char *TAG = "AudioApp";

static lv_obj_t *audio_root = NULL;

static void audio_input_handler(InputEvent *event) {
    if (event->type == INPUT_TYPE_JOYSTICK) {
        if (event->data.joystick_index == 0 || event->data.joystick_index == 2) {
            display_manager_switch_view(&apps_menu_view);
        }
    } else if (event->type == INPUT_TYPE_KEYBOARD) {
        if (event->data.key_value == LV_KEY_ESC || event->data.key_value == '`') {
            display_manager_switch_view(&apps_menu_view);
        }
    }
#ifdef CONFIG_USE_ENCODER
    else if (event->type == INPUT_TYPE_EXIT_BUTTON) {
        display_manager_switch_view(&apps_menu_view);
    }
#endif
}

static void get_audio_callback(void **callback) {
    *callback = audio_input_handler;
}

void audio_create(void) {
    ESP_LOGI(TAG, "Audio app not available on this config");

    display_manager_fill_screen(lv_color_black());

    audio_root = gui_screen_create_root(NULL, "Audio", lv_color_black(), LV_OPA_TRANSP);
    audio_view.root = audio_root;

    lv_obj_t *label = lv_label_create(audio_root);
    lv_label_set_text(label, "Audio not available\non this device");
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
}

void audio_destroy(void) {
    lvgl_obj_del_safe(&audio_root);
    audio_view.root = NULL;
}

View audio_view = {
    .root = NULL,
    .create = audio_create,
    .destroy = audio_destroy,
    .input_callback = audio_input_handler,
    .name = "Audio",
    .get_hardwareinput_callback = get_audio_callback,
};

#endif /* CONFIG_IDF_TARGET_ESP32C5 */
#endif /* CONFIG_BUILD_CONFIG_TEMPLATE */
