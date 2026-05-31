#include "managers/views/audio_player_screen.h"
#include "managers/audio_stream_manager.h"
#include "managers/views/app_gallery_screen.h"
#include "managers/display_manager.h"
#include "managers/sd_card_manager.h"
#include "managers/settings_manager.h"
#include "gui/accessibility_fonts.h"
#include "gui/theme_palette_api.h"
#include "gui/screen_layout.h"
#include "gui/lvgl_safe.h"
#include "gui/toast.h"
#include "lvgl.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#ifdef CONFIG_HAS_TLV320DAC_I2C
#include "io_manager.h"
#include "tlv320dac3100.h"
#endif
#include "freertos/task.h"

#ifdef CONFIG_HAS_AUDIO_PLAYER

static const char *TAG = "AudioPlayer";

/* UI element handles */
static lv_obj_t *s_root = NULL;
static lv_obj_t *s_file_list = NULL;
static lv_obj_t *s_status_label = NULL;
static lv_obj_t *s_progress_bar = NULL;
static lv_obj_t *s_play_btn = NULL;
static lv_obj_t *s_pause_btn = NULL;
static lv_obj_t *s_prev_btn = NULL;
static lv_obj_t *s_next_btn = NULL;

/* State */
static int s_selected_index = 0;
static int s_visible_count = 0;
static uint8_t s_volume_percent = 100;
static lv_timer_t *s_update_timer = NULL;

static bool s_touch_started = false;
static int s_touch_start_x = 0;
static int s_touch_start_y = 0;

static lv_color_t s_bg_color;
static lv_color_t s_surface_color;
static lv_color_t s_surface_alt_color;
static lv_color_t s_text_color;
static lv_color_t s_dim_color;
static lv_color_t s_accent_color;

/* Forward declarations */
static void refresh_theme_colors(void);
static void create_file_list(void);
static void update_file_list_selection(void);
static void audio_player_update_status(void);
static void on_play_clicked(lv_event_t *e);
static void on_pause_clicked(lv_event_t *e);
static void on_prev_clicked(lv_event_t *e);
static void on_next_clicked(lv_event_t *e);
static void update_timer_cb(lv_timer_t *timer);
static void return_to_apps(void);
static void adjust_volume(int delta);

static void refresh_theme_colors(void)
{
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    s_bg_color = lv_color_hex(theme_palette_get_background(theme));
    s_surface_color = lv_color_hex(theme_palette_get_surface(theme));
    s_surface_alt_color = lv_color_hex(theme_palette_get_surface_alt(theme));
    s_text_color = lv_color_hex(theme_palette_get_text(theme));
    s_dim_color = lv_color_hex(theme_palette_get_text_muted(theme));
    s_accent_color = lv_color_hex(theme_palette_get_accent(theme));
}

static void style_card(lv_obj_t *obj, bool selected)
{
    lv_obj_set_style_bg_color(obj, selected ? s_surface_alt_color : s_surface_color, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(obj, selected ? s_accent_color : s_surface_alt_color, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_border_side(obj, selected ? LV_BORDER_SIDE_LEFT : LV_BORDER_SIDE_NONE, 0);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, LV_VER_RES <= 100 ? 3 : 6, 0);
}

static lv_obj_t *create_label(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    return label;
}

static void check_sd_and_show_toast(void)
{
    if (audio_stream_manager_get_file_count() > 0) {
        return;
    }
    if (!audio_stream_manager_sd_available()) {
        toast_show("No SD card - insert card with MP3s in /audio", TOAST_WARN);
    } else {
        toast_show("No MP3 files found in /audio", TOAST_INFO);
    }
}

static void audio_player_update_status(void)
{
    if (!s_status_label) return;

    audio_stream_state_t state = audio_stream_manager_get_state();
    int current = audio_stream_manager_get_current_index();
    int total = audio_stream_manager_get_file_count();

    /* Sync play/pause buttons with current playback state */
    bool is_playing = (state == AUDIO_STREAM_STATE_PLAYING);
    if (s_play_btn) {
        if (is_playing) lv_obj_add_flag(s_play_btn, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_clear_flag(s_play_btn, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_pause_btn) {
        if (is_playing) lv_obj_clear_flag(s_pause_btn, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_pause_btn, LV_OBJ_FLAG_HIDDEN);
    }

    const char *state_str = "Stopped";
    switch (state) {
        case AUDIO_STREAM_STATE_PLAYING: state_str = "Playing"; break;
        case AUDIO_STREAM_STATE_PAUSED:  state_str = "Paused";  break;
        case AUDIO_STREAM_STATE_STOPPED: state_str = "Stopped"; break;
        default: break;
    }

    char buf[96];
    if (total > 0 && current >= 0 && current < total) {
        const char *fname = audio_stream_manager_get_filename(current);
        snprintf(buf, sizeof(buf), "%s  %d/%d  Vol %u%%  %s",
                 state_str, current + 1, total, (unsigned)s_volume_percent,
                 fname ? fname : "");
    } else {
        snprintf(buf, sizeof(buf), "%s  No files", state_str);
    }

    lv_label_set_text(s_status_label, buf);
}

static void create_file_list(void)
{
    int file_count = audio_stream_manager_get_file_count();
    s_visible_count = file_count;

    if (!s_file_list) return;

    /* Clear existing list */
    lv_obj_clean(s_file_list);

    if (file_count == 0) {
        lv_obj_t *lbl = lv_label_create(s_file_list);
        lv_label_set_text(lbl, "No MP3 files found");
        lv_obj_set_style_text_color(lbl, s_dim_color, 0);
        lv_obj_set_style_text_font(lbl, accessibility_get_font_body(), 0);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
        return;
    }

    for (int i = 0; i < file_count; i++) {
        const char *fname = audio_stream_manager_get_filename(i);
        if (!fname) continue;

        lv_obj_t *btn = lv_btn_create(s_file_list);
        lv_obj_set_width(btn, LV_PCT(100));
        lv_obj_set_height(btn, 36);
        style_card(btn, i == s_selected_index);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLL_ON_FOCUS);

        /* Pass index as event user data */
        lv_obj_add_event_cb(btn, on_play_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, fname);
        lv_obj_set_style_text_color(lbl, (i == s_selected_index) ? s_accent_color : s_text_color, 0);
        lv_obj_set_style_text_font(lbl, accessibility_get_font_small(), 0);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(lbl, LV_PCT(96));
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 4, 0);
    }
}

static void update_file_list_selection(void)
{
    if (!s_file_list) return;

    uint32_t child_cnt = lv_obj_get_child_cnt(s_file_list);
    for (uint32_t i = 0; i < child_cnt; i++) {
        lv_obj_t *btn = lv_obj_get_child(s_file_list, i);
        if (!btn) continue;
        bool selected = ((int)i == s_selected_index);
        style_card(btn, selected);
        lv_obj_t *lbl = lv_obj_get_child(btn, 0);
        if (lbl) {
            lv_obj_set_style_text_color(lbl, selected ? s_accent_color : s_text_color, 0);
        }
    }

    /* Scroll selected into view */
    if (s_selected_index >= 0 && s_selected_index < (int)child_cnt) {
        lv_obj_t *btn = lv_obj_get_child(s_file_list, s_selected_index);
        if (btn) lv_obj_scroll_to_view(btn, LV_ANIM_ON);
    }
}

static void on_play_clicked(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0) {
        /* Controls play button: use current selection */
        idx = s_selected_index;
    }
    if (idx < 0 || idx >= s_visible_count) return;

    s_selected_index = idx;
    update_file_list_selection();

    audio_stream_manager_play(idx);
    audio_player_update_status();

    /* Show play/pause buttons */
    if (s_play_btn) lv_obj_add_flag(s_play_btn, LV_OBJ_FLAG_HIDDEN);
    if (s_pause_btn) lv_obj_clear_flag(s_pause_btn, LV_OBJ_FLAG_HIDDEN);
}

static void on_pause_clicked(lv_event_t *e)
{
    (void)e;
    audio_stream_state_t state = audio_stream_manager_get_state();
    if (state == AUDIO_STREAM_STATE_PLAYING) {
        audio_stream_manager_pause();
        if (s_play_btn) lv_obj_clear_flag(s_play_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_pause_btn) lv_obj_add_flag(s_pause_btn, LV_OBJ_FLAG_HIDDEN);
    } else if (state == AUDIO_STREAM_STATE_PAUSED) {
        audio_stream_manager_resume();
        if (s_play_btn) lv_obj_add_flag(s_play_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_pause_btn) lv_obj_clear_flag(s_pause_btn, LV_OBJ_FLAG_HIDDEN);
    }
    audio_player_update_status();
}

static void on_prev_clicked(lv_event_t *e)
{
    (void)e;
    audio_stream_manager_prev();
    s_selected_index = audio_stream_manager_get_current_index();
    update_file_list_selection();
    audio_player_update_status();
}

static void on_next_clicked(lv_event_t *e)
{
    (void)e;
    audio_stream_manager_next();
    s_selected_index = audio_stream_manager_get_current_index();
    update_file_list_selection();
    audio_player_update_status();
}

static void update_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    audio_player_update_status();
}

static void return_to_apps(void)
{
    display_manager_switch_view(&apps_menu_view);
}

static void adjust_volume(int delta)
{
#ifdef CONFIG_HAS_TLV320DAC_I2C
    int volume = (int)s_volume_percent + delta;
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    if (volume == (int)s_volume_percent) return;

    esp_err_t ret = tlv320dac3100_set_volume((uint8_t)volume);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Volume set failed: %s", esp_err_to_name(ret));
        return;
    }
    s_volume_percent = (uint8_t)volume;
    ESP_LOGI(TAG, "Audio volume: %u%%", (unsigned)s_volume_percent);
    audio_player_update_status();
#else
    (void)delta;
#endif
}

void audio_player_create(void)
{
    refresh_theme_colors();

#ifdef CONFIG_HAS_TLV320DAC_I2C
    /* Force full reset of TLV320DAC3100 on each app entry */
    ESP_LOGI(TAG, "Resetting TLV320DAC3100 DAC");
    if (tlv320dac3100_is_initialized()) {
        tlv320dac3100_deinit();
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    esp_err_t reset_ret = io_manager_dac_reset_pulse();
    if (reset_ret != ESP_OK) {
        ESP_LOGW(TAG, "DAC reset pulse failed: %s", esp_err_to_name(reset_ret));
    }
    vTaskDelay(pdMS_TO_TICKS(50));

    tlv320dac3100_config_t dac_cfg = TLV320DAC3100_DEFAULT_CONFIG();
    esp_err_t dac_ret = tlv320dac3100_init(&dac_cfg);
    if (dac_ret != ESP_OK) {
        ESP_LOGW(TAG, "TLV320DAC init failed: %s", esp_err_to_name(dac_ret));
    } else {
        s_volume_percent = 100;
    }
#endif

    /* Initialize audio stream manager (scans SD card for MP3s) */
    esp_err_t ret = audio_stream_manager_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Audio stream manager init failed: %s", esp_err_to_name(ret));
    }

    /* Create root container */
    s_root = gui_screen_create_root(NULL, "Audio Player", s_bg_color, LV_OPA_COVER);
    audio_player_view.root = s_root;
    lv_obj_set_size(s_root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(s_root, 0, 0);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);

    int status_bar_h = GUI_STATUS_BAR_H;
    int screen_h = LV_VER_RES - status_bar_h;
    int controls_h = (screen_h > 200) ? 60 : 48;
    int list_h = screen_h - controls_h - 8;

    display_manager_add_status_bar("Audio Player");

    /* File list container */
    s_file_list = lv_obj_create(s_root);
    lv_obj_set_size(s_file_list, LV_HOR_RES - 8, list_h - 18);
    lv_obj_align(s_file_list, LV_ALIGN_TOP_MID, 0, status_bar_h + 4);
    lv_obj_set_style_bg_opa(s_file_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_file_list, 0, 0);
    lv_obj_set_style_pad_all(s_file_list, 4, 0);
    lv_obj_set_flex_flow(s_file_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_file_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(s_file_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_scroll_dir(s_file_list, LV_DIR_VER);
    lv_obj_set_style_pad_row(s_file_list, 2, 0);
    lv_obj_clear_flag(s_file_list, LV_OBJ_FLAG_CLICKABLE);

    create_file_list();

    /* Controls container */
    lv_obj_t *controls = lv_obj_create(s_root);
    lv_obj_set_size(controls, LV_HOR_RES - 8, controls_h);
    lv_obj_align(controls, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_set_style_bg_opa(controls, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(controls, 0, 0);
    lv_obj_set_style_pad_all(controls, 4, 0);
    lv_obj_clear_flag(controls, LV_OBJ_FLAG_SCROLLABLE);

    /* Previous button */
    s_prev_btn = lv_btn_create(controls);
    lv_obj_set_size(s_prev_btn, 48, 34);
    lv_obj_align(s_prev_btn, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_set_style_bg_color(s_prev_btn, s_surface_alt_color, 0);
    lv_obj_set_style_radius(s_prev_btn, 10, 0);
    lv_obj_set_style_border_width(s_prev_btn, 0, 0);
    lv_obj_set_style_shadow_width(s_prev_btn, 0, 0);
    lv_obj_add_event_cb(s_prev_btn, on_prev_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *prev_lbl = create_label(s_prev_btn, LV_SYMBOL_LEFT, accessibility_get_font_body(), s_accent_color);
    lv_obj_center(prev_lbl);

    /* Play button */
    s_play_btn = lv_btn_create(controls);
    lv_obj_set_size(s_play_btn, 52, 34);
    lv_obj_align(s_play_btn, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_play_btn, s_surface_alt_color, 0);
    lv_obj_set_style_radius(s_play_btn, 10, 0);
    lv_obj_set_style_border_width(s_play_btn, 0, 0);
    lv_obj_set_style_shadow_width(s_play_btn, 0, 0);
    lv_obj_add_event_cb(s_play_btn, on_play_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)-1);
    lv_obj_t *play_lbl = create_label(s_play_btn, LV_SYMBOL_PLAY, accessibility_get_font_body(), s_accent_color);
    lv_obj_center(play_lbl);

    /* Pause button (initially hidden) */
    s_pause_btn = lv_btn_create(controls);
    lv_obj_set_size(s_pause_btn, 52, 34);
    lv_obj_align(s_pause_btn, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_pause_btn, s_surface_alt_color, 0);
    lv_obj_set_style_radius(s_pause_btn, 10, 0);
    lv_obj_set_style_border_width(s_pause_btn, 0, 0);
    lv_obj_set_style_shadow_width(s_pause_btn, 0, 0);
    lv_obj_add_event_cb(s_pause_btn, on_pause_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_pause_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *pause_lbl = create_label(s_pause_btn, "II", accessibility_get_font_body(), s_accent_color);
    lv_obj_center(pause_lbl);

    /* Next button */
    s_next_btn = lv_btn_create(controls);
    lv_obj_set_size(s_next_btn, 48, 34);
    lv_obj_align(s_next_btn, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_set_style_bg_color(s_next_btn, s_surface_alt_color, 0);
    lv_obj_set_style_radius(s_next_btn, 10, 0);
    lv_obj_set_style_border_width(s_next_btn, 0, 0);
    lv_obj_set_style_shadow_width(s_next_btn, 0, 0);
    lv_obj_add_event_cb(s_next_btn, on_next_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *next_lbl = create_label(s_next_btn, LV_SYMBOL_RIGHT, accessibility_get_font_body(), s_accent_color);
    lv_obj_center(next_lbl);

    /* Status label */
    s_status_label = lv_label_create(s_root);
    lv_obj_set_style_text_color(s_status_label, s_dim_color, 0);
    lv_obj_set_style_text_font(s_status_label, accessibility_get_font_small(), 0);
    lv_obj_set_width(s_status_label, LV_HOR_RES - 12);
    lv_label_set_long_mode(s_status_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(s_status_label, LV_ALIGN_BOTTOM_MID, 0, -controls_h - 2);
    lv_label_set_text(s_status_label, "Ready");

    /* Update timer */
    s_update_timer = lv_timer_create(update_timer_cb, 500, NULL);

    /* Check SD card and show toast if needed */
    check_sd_and_show_toast();

    audio_player_update_status();

    ESP_LOGI(TAG, "Audio player view created (%d files)", s_visible_count);
}

void audio_player_destroy(void)
{
    if (s_update_timer) {
        lvgl_timer_del_safe(&s_update_timer);
    }

    audio_stream_manager_stop();
    audio_stream_manager_deinit();

    lvgl_obj_del_safe(&s_root);
    audio_player_view.root = NULL;

    s_file_list = NULL;
    s_status_label = NULL;
    s_progress_bar = NULL;
    s_play_btn = NULL;
    s_pause_btn = NULL;
    s_prev_btn = NULL;
    s_next_btn = NULL;
    s_selected_index = 0;
    s_visible_count = 0;
    s_touch_started = false;
}

static void audio_player_input_handler(InputEvent *event)
{
    if (!event) return;

    if (event->type == INPUT_TYPE_TOUCH) {
        lv_indev_data_t *d = &event->data.touch_data;
        if (d->state == LV_INDEV_STATE_PR && !s_touch_started) {
            s_touch_started = true;
            s_touch_start_x = d->point.x;
            s_touch_start_y = d->point.y;
            return;
        }
        if (d->state == LV_INDEV_STATE_REL && s_touch_started) {
            s_touch_started = false;
            int dx = (int)d->point.x - s_touch_start_x;
            int dy = (int)d->point.y - s_touch_start_y;

            /* Check control button hit areas first (bottom bar) */
            int status_bar_h = GUI_STATUS_BAR_H;
            int screen_h = LV_VER_RES - status_bar_h;
            int controls_h = (screen_h > 200) ? 60 : 48;
            int controls_bottom = LV_VER_RES;
            int controls_top = controls_bottom - controls_h;
            bool in_controls = (d->point.y >= controls_top && d->point.y <= controls_bottom);

            if (in_controls && abs(dx) < 15 && abs(dy) < 15) {
                /* Tap on control buttons */
                int cx = d->point.x;
                int btn_w = 48;
                int btn_spacing = 8;
                int center_x = LV_HOR_RES / 2;

                /* Prev button: left side */
                int prev_left = btn_spacing;
                int prev_right = prev_left + btn_w;
                if (cx >= prev_left && cx <= prev_right) {
                    audio_stream_manager_prev();
                    s_selected_index = audio_stream_manager_get_current_index();
                    update_file_list_selection();
                    audio_player_update_status();
                    return;
                }

                /* Play/Pause button: center */
                int play_left = center_x - btn_w / 2;
                int play_right = center_x + btn_w / 2;
                if (cx >= play_left && cx <= play_right) {
                    audio_stream_state_t st = audio_stream_manager_get_state();
                    if (st == AUDIO_STREAM_STATE_PLAYING) {
                        audio_stream_manager_pause();
                    } else {
                        if (s_selected_index >= 0 && s_selected_index < s_visible_count) {
                            audio_stream_manager_play(s_selected_index);
                        }
                    }
                    audio_player_update_status();
                    return;
                }

                /* Next button: right side */
                int next_right = LV_HOR_RES - btn_spacing;
                int next_left = next_right - btn_w;
                if (cx >= next_left && cx <= next_right) {
                    audio_stream_manager_next();
                    s_selected_index = audio_stream_manager_get_current_index();
                    update_file_list_selection();
                    audio_player_update_status();
                    return;
                }
                return;
            }

            if (abs(dx) < 10 && abs(dy) < 10) {
                /* Tap on list item */
                uint32_t child_cnt = lv_obj_get_child_cnt(s_file_list);
                for (uint32_t i = 0; i < child_cnt; i++) {
                    lv_obj_t *child = lv_obj_get_child(s_file_list, i);
                    if (!child) continue;
                    lv_area_t a;
                    lv_obj_get_coords(child, &a);
                    if (d->point.x >= a.x1 && d->point.x <= a.x2 &&
                        d->point.y >= a.y1 && d->point.y <= a.y2) {
                        s_selected_index = (int)i;
                        update_file_list_selection();
                        audio_stream_manager_play((int)i);
                        audio_player_update_status();
                        break;
                    }
                }
            } else if (abs(dx) < 40 && abs(dy) > 20) {
                /* Vertical swipe: move selection */
                if (dy < 0 && s_selected_index > 0) {
                    s_selected_index--;
                    update_file_list_selection();
                } else if (dy > 0 && s_selected_index < s_visible_count - 1) {
                    s_selected_index++;
                    update_file_list_selection();
                }
            }
        }
        return;
    }

    if (event->type == INPUT_TYPE_JOYSTICK) {
        int btn = event->data.joystick_index;
        /* Map joystick: 0=left, 1=select, 2=up, 3=right, 4=down */
        if (btn == 0) { /* Left -> exit */
            return_to_apps();
        } else if (btn == 2) { /* Up */
            if (s_selected_index > 0) {
                s_selected_index--;
                update_file_list_selection();
            }
        } else if (btn == 4) { /* Down */
            if (s_selected_index < s_visible_count - 1) {
                s_selected_index++;
                update_file_list_selection();
            }
        } else if (btn == 1) { /* Select */
            if (s_selected_index >= 0 && s_selected_index < s_visible_count) {
                audio_stream_manager_play(s_selected_index);
                audio_player_update_status();
            }
        } else if (btn == 3) { /* Right -> next track */
            audio_stream_manager_next();
            s_selected_index = audio_stream_manager_get_current_index();
            update_file_list_selection();
            audio_player_update_status();
        }
        return;
    }

    if (event->type == INPUT_TYPE_ENCODER) {
        if (event->data.encoder.button) {
            /* Encoder button press = play selected */
            if (s_selected_index >= 0 && s_selected_index < s_visible_count) {
                audio_stream_manager_play(s_selected_index);
                audio_player_update_status();
            }
        } else {
            adjust_volume(event->data.encoder.direction > 0 ? 5 : -5);
        }
        return;
    }

    if (event->type == INPUT_TYPE_EXIT_BUTTON) {
        return_to_apps();
        return;
    }

    if (event->type == INPUT_TYPE_KEYBOARD) {
        int key = event->data.key_value;
        if (key == LV_KEY_ESC || key == '`') {
            return_to_apps();
        } else if (key == LV_KEY_UP || key == 'k') {
            if (s_selected_index > 0) {
                s_selected_index--;
                update_file_list_selection();
            }
        } else if (key == LV_KEY_DOWN || key == 'j') {
            if (s_selected_index < s_visible_count - 1) {
                s_selected_index++;
                update_file_list_selection();
            }
        } else if (key == LV_KEY_ENTER || key == 13) {
            if (s_selected_index >= 0 && s_selected_index < s_visible_count) {
                audio_stream_manager_play(s_selected_index);
                audio_player_update_status();
            }
        }
        return;
    }
}

static void get_audio_player_callback(void **callback)
{
    *callback = audio_player_input_handler;
}

View audio_player_view = {
    .root = NULL,
    .create = audio_player_create,
    .destroy = audio_player_destroy,
    .input_callback = audio_player_input_handler,
    .name = "Audio Player",
    .get_hardwareinput_callback = get_audio_player_callback,
};

#else /* !CONFIG_HAS_AUDIO_PLAYER */

void audio_player_create(void) {}
void audio_player_destroy(void) {}

static void audio_player_dummy_handler(InputEvent *event) { (void)event; }
static void get_audio_player_callback(void **callback) { *callback = audio_player_dummy_handler; }

View audio_player_view = {
    .root = NULL,
    .create = audio_player_create,
    .destroy = audio_player_destroy,
    .input_callback = audio_player_dummy_handler,
    .name = "Audio Player",
    .get_hardwareinput_callback = get_audio_player_callback,
};

#endif /* CONFIG_HAS_AUDIO_PLAYER */
