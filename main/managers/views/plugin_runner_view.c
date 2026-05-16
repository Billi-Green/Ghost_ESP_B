#include "managers/views/plugin_runner_view.h"

#include "gui/lvgl_safe.h"
#include "gui/screen_layout.h"
#include "managers/plugin_api.h"
#include "managers/plugin_loader.h"
#include "managers/sd_card_manager.h"
#include "managers/views/app_gallery_screen.h"
#include "managers/views/error_popup.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "PluginRunner";

static char s_pending_app_id[PLUGIN_APP_ID_MAX];
static lv_obj_t *s_root = NULL;
static lv_obj_t *s_title = NULL;
static lv_obj_t *s_output = NULL;
static lv_timer_t *s_tick_timer = NULL;
static char s_output_buf[2048];

#define PLUGIN_RUNNER_TICK_MS 100

typedef enum {
    RUNNER_UI_SET_TITLE,
    RUNNER_UI_PRINT,
    RUNNER_UI_CLEAR,
    RUNNER_UI_TOAST,
} runner_ui_action_type_t;

typedef struct {
    runner_ui_action_type_t type;
    char text[256];
} runner_ui_action_t;

static void runner_set_title_now(const char *title) {
    if (s_title && lv_obj_is_valid(s_title)) {
        lv_label_set_text(s_title, title ? title : "SD App");
    }
}

static void runner_print_now(const char *text) {
    if (!s_output || !lv_obj_is_valid(s_output) || !text) return;
    size_t cur = strlen(s_output_buf);
    size_t add = strlen(text);
    if (cur + add + 2 >= sizeof(s_output_buf)) {
        memmove(s_output_buf, s_output_buf + sizeof(s_output_buf) / 2, strlen(s_output_buf + sizeof(s_output_buf) / 2) + 1);
        cur = strlen(s_output_buf);
    }
    snprintf(s_output_buf + cur, sizeof(s_output_buf) - cur, "%s", text);
    lv_label_set_text(s_output, s_output_buf);
}

static void runner_clear_now(void) {
    s_output_buf[0] = '\0';
    if (s_output && lv_obj_is_valid(s_output)) lv_label_set_text(s_output, "");
}

static void runner_ui_apply(void *arg) {
    runner_ui_action_t *action = (runner_ui_action_t *)arg;
    if (!action) return;
    switch (action->type) {
        case RUNNER_UI_SET_TITLE:
            runner_set_title_now(action->text);
            break;
        case RUNNER_UI_PRINT:
            runner_print_now(action->text);
            break;
        case RUNNER_UI_CLEAR:
            runner_clear_now();
            break;
        case RUNNER_UI_TOAST:
            runner_print_now(action->text);
            runner_print_now("\n");
            break;
    }
    free(action);
}

static void runner_post_ui(runner_ui_action_type_t type, const char *text) {
    runner_ui_action_t *action = calloc(1, sizeof(*action));
    if (!action) return;
    action->type = type;
    if (text) {
        strncpy(action->text, text, sizeof(action->text) - 1);
    }
    display_manager_run_on_lvgl(runner_ui_apply, action);
}

static void runner_api_set_title(const char *title) {
    runner_post_ui(RUNNER_UI_SET_TITLE, title);
}

static void runner_api_print(const char *text) {
    runner_post_ui(RUNNER_UI_PRINT, text);
}

static void runner_api_clear(void) {
    runner_post_ui(RUNNER_UI_CLEAR, NULL);
}

static void runner_api_toast(const char *message) {
    runner_post_ui(RUNNER_UI_TOAST, message);
}

static bool s_sd_eject_detected = false;

static void plugin_runner_tick_cb(lv_timer_t *timer) {
    (void)timer;
    if (s_sd_eject_detected) return;
    plugin_loaded_app_t *loaded = plugin_loader_current();
    if (!loaded || !loaded->running || loaded->state != PLUGIN_APP_STATE_RUNNING) return;
    if (!sd_card_manager.is_initialized) {
        ESP_LOGW(TAG, "SD card removed while app running, stopping");
        s_sd_eject_detected = true;
        if (loaded->app && loaded->app->on_stop) loaded->app->on_stop();
        loaded->running = false;
        loaded->state = PLUGIN_APP_STATE_LOADED;
        display_manager_switch_view(&apps_menu_view);
        return;
    }
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    uint32_t elapsed_ms = loaded->last_tick_ms ? now_ms - loaded->last_tick_ms : PLUGIN_RUNNER_TICK_MS;
    loaded->last_tick_ms = now_ms;
    plugin_loader_tick(loaded, elapsed_ms);
}

void plugin_runner_set_app(const char *app_id) {
    s_pending_app_id[0] = '\0';
    if (app_id) {
        strncpy(s_pending_app_id, app_id, sizeof(s_pending_app_id) - 1);
    }
}

static ghostesp_input_event_t convert_input(const InputEvent *event) {
    ghostesp_input_event_t out = { .type = GHOSTESP_INPUT_NONE };
    if (!event) return out;
    switch (event->type) {
        case INPUT_TYPE_JOYSTICK:
            out.pressed = event->data.joystick_pressed;
            switch (event->data.joystick_index) {
                case 0: out.type = GHOSTESP_INPUT_LEFT; break;
                case 1: out.type = GHOSTESP_INPUT_SELECT; break;
                case 2: out.type = GHOSTESP_INPUT_UP; break;
                case 3: out.type = GHOSTESP_INPUT_RIGHT; break;
                case 4: out.type = GHOSTESP_INPUT_DOWN; break;
                default: out.type = GHOSTESP_INPUT_NONE; break;
            }
            break;
        case INPUT_TYPE_KEYBOARD:
            out.type = GHOSTESP_INPUT_KEY;
            out.value = event->data.key_value;
            if (event->data.key_value == LV_KEY_ESC || event->data.key_value == '`') out.type = GHOSTESP_INPUT_BACK;
            break;
        case INPUT_TYPE_TOUCH:
            out.type = GHOSTESP_INPUT_TOUCH;
            out.x = event->data.touch_data.point.x;
            out.y = event->data.touch_data.point.y;
            out.pressed = event->data.touch_data.state == LV_INDEV_STATE_PR;
            break;
        case INPUT_TYPE_ENCODER:
            out.type = event->data.encoder.button ? GHOSTESP_INPUT_SELECT : (event->data.encoder.direction > 0 ? GHOSTESP_INPUT_RIGHT : GHOSTESP_INPUT_LEFT);
            out.value = event->data.encoder.direction;
            break;
        case INPUT_TYPE_EXIT_BUTTON:
            out.type = GHOSTESP_INPUT_BACK;
            out.pressed = event->data.exit_pressed;
            break;
    }
    return out;
}

static void plugin_runner_event_handler(InputEvent *event) {
    if (!event) return;
    ghostesp_input_event_t app_event = convert_input(event);
    if (app_event.type == GHOSTESP_INPUT_BACK) {
        display_manager_switch_view(&apps_menu_view);
        return;
    }
    plugin_loaded_app_t *loaded = plugin_loader_current();
    if (loaded && loaded->running && loaded->app && loaded->app->on_input) {
        loaded->app->on_input(&app_event);
    }
}

void plugin_runner_view_create(void) {
    if (s_tick_timer) {
        lv_timer_del(s_tick_timer);
        s_tick_timer = NULL;
    }
    s_sd_eject_detected = false;
    s_output_buf[0] = '\0';
    s_root = gui_screen_create_root(NULL, "SD App", lv_color_hex(0x121212), LV_OPA_COVER);
    plugin_runner_view.root = s_root;
    display_manager_add_status_bar("SD App");

    lv_obj_t *content = gui_screen_create_content(s_root, GUI_STATUS_BAR_HEIGHT);
    lv_obj_set_style_pad_all(content, 8, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    s_title = lv_label_create(content);
    lv_label_set_text(s_title, "Loading SD app...");
    lv_obj_set_style_text_color(s_title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(s_title, &lv_font_montserrat_16, 0);

    s_output = lv_label_create(content);
    lv_label_set_text(s_output, "");
    lv_label_set_long_mode(s_output, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_output, LV_PCT(100));
    lv_obj_set_style_text_color(s_output, lv_color_hex(0xD0D0D0), 0);
    lv_obj_set_style_text_font(s_output, &lv_font_montserrat_12, 0);

    plugin_api_set_ui_hooks(runner_api_set_title, runner_api_print, runner_api_clear, runner_api_toast);

    if (s_pending_app_id[0] == '\0') {
        runner_set_title_now("No app selected");
        runner_print_now("Return to Apps and select an SD app.\n");
        return;
    }

    plugin_loaded_app_t *loaded = NULL;
    esp_err_t err = plugin_loader_load(s_pending_app_id, &loaded);
    if (err != ESP_OK) {
        runner_set_title_now("Launch Failed");
        runner_print_now(plugin_loader_last_error());
        runner_print_now("\n");
        return;
    }

    const plugin_app_manifest_t *manifest = loaded->manifest;
    runner_set_title_now(manifest ? manifest->name : "SD App");
    ESP_LOGI(TAG, "Starting app %s", s_pending_app_id);
    plugin_loader_start(loaded);
    if (loaded->app && loaded->app->on_tick && !s_tick_timer) {
        s_tick_timer = lv_timer_create(plugin_runner_tick_cb, PLUGIN_RUNNER_TICK_MS, NULL);
    }
}

void plugin_runner_stop_tick(void) {
    if (s_tick_timer) {
        lv_timer_del(s_tick_timer);
        s_tick_timer = NULL;
    }
}

void plugin_runner_view_destroy(void) {
    plugin_runner_stop_tick();
    plugin_loader_unload(plugin_loader_current());
    plugin_api_set_ui_hooks(NULL, NULL, NULL, NULL);
    lvgl_obj_del_safe(&s_root);
    s_title = NULL;
    s_output = NULL;
    plugin_runner_view.root = NULL;
}

void plugin_runner_get_callback(void **callback) {
    *callback = plugin_runner_event_handler;
}

View plugin_runner_view = {
    .root = NULL,
    .create = plugin_runner_view_create,
    .destroy = plugin_runner_view_destroy,
    .input_callback = plugin_runner_event_handler,
    .name = "SD App",
    .get_hardwareinput_callback = plugin_runner_get_callback,
};
