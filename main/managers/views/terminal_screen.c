#include "managers/views/terminal_screen.h"
#include "core/serial_manager.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "managers/views/main_menu_screen.h"
#include "managers/wifi_manager.h"
#include "managers/display_manager.h"
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "managers/settings_manager.h"

static const char *TAG = "Terminal";
static lv_obj_t *terminal_page = NULL;
static SemaphoreHandle_t terminal_mutex = NULL;
static bool terminal_active = false;
static bool is_stopping = false;
#define MAX_TEXT_LENGTH 4096
#define CLEANUP_THRESHOLD (MAX_TEXT_LENGTH * 3 / 4)
#define CLEANUP_AMOUNT (MAX_TEXT_LENGTH / 2)
#define MAX_QUEUE_SIZE 10
#define MAX_MESSAGE_SIZE 256
#define MIN_SCREEN_SIZE 239
#define BUTTON_SIZE 40
#define BUTTON_PADDING 5

static lv_obj_t *back_btn = NULL;
lv_timer_t *terminal_update_timer = NULL;

static void scroll_terminal_up(void);
static void scroll_terminal_down(void);
static void stop_all_operations(void);


typedef struct {
  char messages[MAX_QUEUE_SIZE][MAX_MESSAGE_SIZE];
  int head;
  int tail;
  int count;
} MessageQueue;

static MessageQueue message_queue = {.head = 0, .tail = 0, .count = 0};


static void queue_message(const char *text) {
  if (message_queue.count >= MAX_QUEUE_SIZE) {
    message_queue.head = (message_queue.head + 1) % MAX_QUEUE_SIZE;
    message_queue.count--;
  }
  strncpy(message_queue.messages[message_queue.tail], text, MAX_MESSAGE_SIZE - 1);
  message_queue.messages[message_queue.tail][MAX_MESSAGE_SIZE - 1] = '\0';
  message_queue.tail = (message_queue.tail + 1) % MAX_QUEUE_SIZE;
  message_queue.count++;
}

static void clear_message_queue(void) {
  message_queue.head = 0;
  message_queue.tail = 0;
  message_queue.count = 0;
}

static void process_queued_messages(void) {
  if (!terminal_active || !terminal_page || is_stopping || message_queue.count == 0) {
    return;
  }

  if (xSemaphoreTake(terminal_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
    ESP_LOGW(TAG, "Failed to acquire terminal mutex in process_queued_messages");
    return; // Try again later
  }

  lv_obj_t *last_item = NULL;
  while (message_queue.count > 0) {
    const char *msg = message_queue.messages[message_queue.head];
    
    // Add text to LVGL list
    lv_obj_t *item = lv_list_add_text(terminal_page, msg);
    lv_label_set_long_mode(item, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_bg_opa(item, LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_color(item, lv_color_hex(settings_get_terminal_text_color(&G_Settings)), 0);
    lv_obj_set_style_text_font(item, &lv_font_montserrat_10, 0);
    last_item = item; // Keep track of the last added item

    // Dequeue
    message_queue.head = (message_queue.head + 1) % MAX_QUEUE_SIZE;
    message_queue.count--;
  }

  // Scroll to the last item added in this batch
  if (last_item) {
    lv_obj_scroll_to_view(last_item, LV_ANIM_OFF);
  }

  xSemaphoreGive(terminal_mutex);
}

// Wrapper callback for the LVGL timer
static void process_queued_messages_callback(lv_timer_t * timer) {
    process_queued_messages();
}

int custom_log_vprintf(const char *fmt, va_list args);
static int (*default_log_vprintf)(const char *, va_list) = NULL;

static void scroll_terminal_up(void) {
  if (!terminal_page) return;
  lv_coord_t scroll_pixels = lv_obj_get_height(terminal_page) / 2;
  lv_obj_scroll_by(terminal_page, 0, scroll_pixels, LV_ANIM_OFF);
  lv_obj_invalidate(terminal_page);
  ESP_LOGI(TAG, "Scroll up triggered");
}

static void scroll_terminal_down(void) {
  if (!terminal_page) return;
  lv_coord_t scroll_pixels = lv_obj_get_height(terminal_page) / 2;
  lv_obj_scroll_by(terminal_page, 0, -scroll_pixels, LV_ANIM_OFF);
  lv_obj_invalidate(terminal_page);
  ESP_LOGI(TAG, "Scroll down triggered");
}

static void stop_all_operations(void) {
  terminal_active = false;
  is_stopping = true;
  clear_message_queue();
  simulateCommand("stop");
  simulateCommand("stopspam");
  simulateCommand("stopdeauth");
  simulateCommand("capture -stop");
  simulateCommand("stopportal");
  simulateCommand("gpsinfo -s");
  simulateCommand("blewardriving -s");
  simulateCommand("pineap -s");
  display_manager_switch_view(&options_menu_view);
  ESP_LOGI(TAG, "Stop all operations triggered");
}

void terminal_view_create(void) {
  is_stopping = false;
  if (terminal_view.root != NULL) {
    return;
  }

  if (!terminal_mutex) {
    terminal_mutex = xSemaphoreCreateMutex();
    if (!terminal_mutex) {
      ESP_LOGE(TAG, "Failed to create terminal mutex");
      return;
    }
  }

  terminal_active = true;

  terminal_view.root = lv_obj_create(lv_scr_act());
  lv_obj_set_size(terminal_view.root, LV_HOR_RES, LV_VER_RES);
  lv_obj_set_style_bg_color(terminal_view.root, lv_color_black(), 0);
  lv_obj_set_scrollbar_mode(terminal_view.root, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_style_pad_all(terminal_view.root, 0, 0);

  // Define status bar height (as seen in display_manager.c)
  const int STATUS_BAR_HEIGHT = 20; 
  
  // Calculate available height, considering status bar and bottom buttons (if present)
  int available_height = LV_VER_RES - STATUS_BAR_HEIGHT;
  if (LV_HOR_RES > MIN_SCREEN_SIZE && LV_VER_RES > MIN_SCREEN_SIZE) {
      available_height -= (BUTTON_SIZE + BUTTON_PADDING * 2);
  }
  int textarea_height = available_height;

  terminal_page = lv_list_create(terminal_view.root);
  // Set position below status bar
  lv_obj_set_pos(terminal_page, 0, STATUS_BAR_HEIGHT); 
  lv_obj_set_size(terminal_page, LV_HOR_RES, textarea_height);
  lv_obj_set_style_bg_color(terminal_page, lv_color_black(), 0);
  lv_obj_set_style_pad_all(terminal_page, 0, 0);
  lv_obj_set_scrollbar_mode(terminal_page, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_style_border_width(terminal_page, 0, 0);
  lv_obj_set_style_clip_corner(terminal_page, false, 0);
  lv_obj_set_scrollbar_mode(terminal_view.root, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_style_border_width(terminal_view.root, 0, 0);
  lv_obj_set_style_radius(terminal_view.root, 0, 0);
  lv_obj_set_scroll_dir(terminal_page, LV_DIR_VER);

  if (LV_HOR_RES > MIN_SCREEN_SIZE && LV_VER_RES > MIN_SCREEN_SIZE) {
    back_btn = lv_btn_create(terminal_view.root);
    lv_obj_set_size(back_btn, BUTTON_SIZE, BUTTON_SIZE);
    lv_obj_align(back_btn, LV_ALIGN_BOTTOM_LEFT, BUTTON_PADDING, -BUTTON_PADDING);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_radius(back_btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_border_width(back_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(back_btn, 0, LV_PART_MAIN);
    lv_obj_t *back_label = lv_label_create(back_btn);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT);
    lv_obj_center(back_label);


    lv_obj_update_layout(terminal_view.root);
    ESP_LOGW(TAG, "Back pos: x=%d, y=%d, w=%d, h=%d", 
             lv_obj_get_x(back_btn), lv_obj_get_y(back_btn), 
             lv_obj_get_width(back_btn), lv_obj_get_height(back_btn));
  }

  display_manager_add_status_bar("Terminal");

  // Create and start the update timer
  if (!terminal_update_timer) { 
      terminal_update_timer = lv_timer_create(process_queued_messages_callback, 50, NULL); // 50ms interval
      if (!terminal_update_timer) {
          ESP_LOGE(TAG, "Failed to create terminal update timer");
      }
  }
}

void terminal_view_destroy(void) {
  terminal_active = false;
  is_stopping = true;
  clear_message_queue();

  // Stop and delete the timer
  if (terminal_update_timer) {
    lv_timer_del(terminal_update_timer);
    terminal_update_timer = NULL;
  }

  vTaskDelay(pdMS_TO_TICKS(50));

  if (terminal_mutex) {
    if (xSemaphoreTake(terminal_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
      vSemaphoreDelete(terminal_mutex);
      terminal_mutex = NULL;
    }
  }

  if (terminal_view.root != NULL) {
    lv_obj_del(terminal_view.root);
    terminal_view.root = NULL;
    terminal_page = NULL;
    back_btn = NULL;
  }

  is_stopping = false;
}


void terminal_view_add_text(const char *text) {
  if (!text || is_stopping) return;
  if (text[0] == '\0') return;

  // If terminal is not active or ready, just queue the message
  if (!terminal_active || !terminal_page || !terminal_mutex) {
    // Need mutex to safely queue even if terminal inactive
    if (!terminal_mutex) { // Create if absolutely needed, should ideally exist
        ESP_LOGW(TAG, "Terminal mutex not yet created, creating temporarily");
        terminal_mutex = xSemaphoreCreateMutex();
        if (!terminal_mutex) {
            ESP_LOGE(TAG, "Failed to create temporary mutex for queueing");
            return; // Cannot queue safely
        }
    }
    if (xSemaphoreTake(terminal_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        queue_message(text);
        xSemaphoreGive(terminal_mutex);
    } else {
        ESP_LOGW(TAG, "Failed to get mutex for early queueing");
        // Maybe drop message or handle error?
    }
    return;
  }

  // Terminal is active, acquire mutex and queue message
  if (xSemaphoreTake(terminal_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
    ESP_LOGW(TAG, "Failed to acquire terminal mutex in add_text");
    // Consider alternative like trying to queue without mutex if desperate?
    // For now, log and drop/ignore.
    return;
  }

  queue_message(text);
  
  xSemaphoreGive(terminal_mutex);
}

void terminal_view_hardwareinput_callback(InputEvent *event) {
  if (event->type == INPUT_TYPE_TOUCH) {
    if (event->data.touch_data.state != LV_INDEV_STATE_PR) {
      return;
    }
    int touch_x = event->data.touch_data.point.x;
    int touch_y = event->data.touch_data.point.y;
    ESP_LOGW(TAG, "Touch detected at x=%d, y=%d (screen: %dx%d)", touch_x, touch_y, LV_HOR_RES, LV_VER_RES);

    if (LV_HOR_RES > MIN_SCREEN_SIZE && LV_VER_RES > MIN_SCREEN_SIZE) {
      int button_y_min = LV_VER_RES - (BUTTON_SIZE + BUTTON_PADDING * 2);
      int button_y_max = LV_VER_RES - BUTTON_PADDING;
      

      if (touch_y >= button_y_min && touch_y <= button_y_max) {
        int back_x_min = BUTTON_PADDING;
        int back_x_max = BUTTON_PADDING + BUTTON_SIZE + 25;
        if (touch_x >= back_x_min && touch_x <= back_x_max) {
          ESP_LOGW(TAG, "Back button triggered");
          stop_all_operations();
          return;
        }
      }
      

      int screen_half = LV_VER_RES / 2;
      if (touch_y < screen_half) {
        ESP_LOGW(TAG, "Top half tap - Scroll up");
        scroll_terminal_up();
      } else if (touch_y < button_y_min) {
        ESP_LOGW(TAG, "Bottom half tap - Scroll down");
        scroll_terminal_down();
      }
    } else {
      int screen_half = LV_VER_RES / 2;
      if (touch_y < screen_half) {
        ESP_LOGW(TAG, "Top half tap - Scroll up (small screen)");
        scroll_terminal_up();
      } else {
        ESP_LOGW(TAG, "Bottom half tap - Scroll down (small screen)");
        scroll_terminal_down();
      }
    }
  } else if (event->type == INPUT_TYPE_JOYSTICK) {
    int button = event->data.joystick_index;
    if (button == 1) {
      ESP_LOGW(TAG, "Joystick button 1: Stop all operations");
      stop_all_operations();
    } else if (button == 2) {
      ESP_LOGW(TAG, "Joystick button 2: Scroll up");
      scroll_terminal_up();
    } else if (button == 4) {
      ESP_LOGW(TAG, "Joystick button 4: Scroll down");
      scroll_terminal_down();
    }
  }
}


void terminal_view_get_hardwareinput_callback(void **callback) {
  if (callback != NULL) {
    *callback = (void *)terminal_view_hardwareinput_callback;
  }
}

int custom_log_vprintf(const char *fmt, va_list args) {
  char buf[256];
  int len = vsnprintf(buf, sizeof(buf), fmt, args);
  if (len < 0) {
    return len;
  }
  terminal_view_add_text(buf);
  return len;
}

View terminal_view = {
  .root = NULL,
  .create = terminal_view_create,
  .destroy = terminal_view_destroy,
  .input_callback = terminal_view_hardwareinput_callback,
  .name = "TerminalView",
  .get_hardwareinput_callback = terminal_view_get_hardwareinput_callback
};