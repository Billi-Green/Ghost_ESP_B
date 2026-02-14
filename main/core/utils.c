#include "core/utils.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_netif.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <sys/dirent.h>
#include <stdio.h>

#define TAG "Utils"

const char *wrap_message(const char *message, const char *file, int line) {
  int size =
      snprintf(NULL, 0, "File: %s, Line: %d, Message: %s", file, line, message);

  char *buffer = (char *)malloc(size + 1);

  if (buffer != NULL) {
    snprintf(buffer, size + 1, "File: %s, Line: %d, Message: %s", file, line,
             message);
  }
  return buffer;
}

void scale_grb_by_brightness(uint8_t *g, uint8_t *r, uint8_t *b, float brightness) {
  *g = (uint8_t)(*g * brightness);
  *r = (uint8_t)(*r * brightness);
  *b = (uint8_t)(*b * brightness);
}

void scale_grb_by_neopixel_brightness(uint8_t *g, uint8_t *r, uint8_t *b, float base_brightness,
                                      uint8_t max_brightness_percent) {
  *g = (uint8_t)(*g * base_brightness);
  *r = (uint8_t)(*r * base_brightness);
  *b = (uint8_t)(*b * base_brightness);

  float neopixel_scale = max_brightness_percent / 100.0f;
  *g = (uint8_t)(*g * neopixel_scale);
  *r = (uint8_t)(*r * neopixel_scale);
  *b = (uint8_t)(*b * neopixel_scale);
}

bool is_in_task_context(void) {
  if (xTaskGetCurrentTaskHandle() != NULL) {
    return true;
  } else {
    return false;
  }
}

void url_decode(char *decoded, const char *encoded) {
  char c;
  int i, j = 0;
  for (i = 0; encoded[i] != '\0'; i++) {
    if (encoded[i] == '%') {
      sscanf(&encoded[i + 1], "%2hhx", &c);
      decoded[j++] = c;
      i += 2;
    } else if (encoded[i] == '+') {
      decoded[j++] = ' ';
    } else {
      decoded[j++] = encoded[i];
    }
  }
  decoded[j] = '\0';
}

int get_query_param_value(const char *query, const char *key, char *value,
                          size_t value_size) {
  char *param_start = strstr(query, key);
  if (param_start) {
    param_start += strlen(key) + 1;
    char *param_end = strchr(param_start, '&');
    if (param_end == NULL) {
      param_end = param_start + strlen(param_start);
    }

    size_t param_len = param_end - param_start;
    if (param_len >= value_size) {
      return ESP_ERR_INVALID_SIZE;
    }
    strncpy(value, param_start, param_len);
    value[param_len] = '\0';
    return ESP_OK;
  }
  return ESP_ERR_NOT_FOUND;
}

int get_next_pcap_file_index(const char *base_name) {
  int max_index = -1;

  DIR *dir = opendir("/mnt/ghostesp/pcaps");
  if (!dir) {
    ESP_LOGE(TAG, "Failed to open directory /mnt/ghostesp/pcaps");
    return -1;
  }

  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {

    if (strncmp(entry->d_name, base_name, strlen(base_name)) == 0) {

      int index;
      if (sscanf(entry->d_name + strlen(base_name), "_%d.pcap", &index) == 1) {

        if (index > max_index) {
          max_index = index;
        }
      }
    }
  }

  closedir(dir);
  return max_index + 1;
}

int get_next_csv_file_index(const char *base_name) {
  int max_index = -1;

  DIR *dir = opendir("/mnt/ghostesp/gps");
  if (!dir) {
    ESP_LOGE(TAG, "Failed to open directory /mnt/ghostesp/gps");
    return -1;
  }

  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (strncmp(entry->d_name, base_name, strlen(base_name)) == 0) {
      int index;
      if (sscanf(entry->d_name + strlen(base_name), "_%d.csv", &index) == 1) {
        if (index > max_index) {
          max_index = index;
        }
      }
    }
  }

  closedir(dir);
  return max_index + 1;
}

int get_next_file_index(const char *dir_path, const char *base_name,
                          const char *extension) {
  int max_index = -1;

  DIR *dir = opendir(dir_path);
  if (!dir) {
    return 0;
  }

  size_t base_len = strlen(base_name);
  struct dirent *entry;

  while ((entry = readdir(dir)) != NULL) {
    if (strncmp(entry->d_name, base_name, base_len) != 0) continue;
    const char *rest = entry->d_name + base_len;
    if (*rest != '_') continue;
    char *end = NULL;
    int index = (int)strtol(rest + 1, &end, 10);
    if (end == rest + 1 || *end != '.') continue;
    if (strcmp(end + 1, extension) != 0) continue;
    if (index > max_index) max_index = index;
  }

  closedir(dir);
  return max_index + 1;
}

void log_heap_status(const char *tag, const char *event) {
  size_t free8 = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  size_t largest8 = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  size_t free32 = heap_caps_get_free_size(MALLOC_CAP_32BIT);
  ESP_LOGI(tag, "[heap] %s free8=%u largest8=%u free32=%u",
           event,
           (unsigned)free8,
           (unsigned)largest8,
           (unsigned)free32);
}

void format_mac_address(const uint8_t *mac, char *buffer, size_t buffer_len, bool uppercase) {
  if (mac == NULL || buffer == NULL || buffer_len < 18) {
    return;
  }

  const char *format = uppercase ? "%02X:%02X:%02X:%02X:%02X:%02X"
                                 : "%02x:%02x:%02x:%02x:%02x:%02x";
  snprintf(buffer,
           buffer_len,
           format,
           mac[0],
           mac[1],
           mac[2],
           mac[3],
           mac[4],
           mac[5]);
}

bool str_copy_upper(char *dst, size_t dst_size, const char *src) {
  if (dst == NULL || src == NULL || dst_size == 0) {
    return false;
  }

  size_t src_len = strlen(src);
  if (src_len + 1 > dst_size) {
    dst[0] = '\0';
    return false;
  }

  for (size_t i = 0; i < src_len; i++) {
    dst[i] = (char)toupper((unsigned char)src[i]);
  }
  dst[src_len] = '\0';
  return true;
}

// ============================================================================
// Network/MAC Utilities
// ============================================================================

void build_ip_string(char *buffer, size_t size, const char *prefix, int host) {
  if (buffer == NULL || prefix == NULL || size == 0) {
    return;
  }
  snprintf(buffer, size, "%s%d", prefix, host);
}

// ============================================================================
// WiFi Network Utilities
// ============================================================================

esp_netif_t *get_wifi_sta_netif(void) {
  return esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
}

bool is_wifi_sta_connected(void) {
  wifi_ap_record_t ap_info;
  return (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK);
}

bool get_own_ip_and_mac(esp_netif_t *netif, esp_netif_ip_info_t *ip_info, uint8_t *mac) {
  if (netif == NULL || ip_info == NULL || mac == NULL) {
    return false;
  }
  
  if (esp_netif_get_ip_info(netif, ip_info) != ESP_OK) {
    return false;
  }
  
  if (esp_netif_get_mac(netif, mac) != ESP_OK) {
    return false;
  }
  
  return true;
}

// ============================================================================
// Byte/Buffer Utilities
// ============================================================================

uint16_t read_u16_le(const uint8_t *data) {
  return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

uint32_t read_u32_le(const uint8_t *data) {
  return (uint32_t)data[0] |
         ((uint32_t)data[1] << 8) |
         ((uint32_t)data[2] << 16) |
         ((uint32_t)data[3] << 24);
}

void parse_ble_device_name(const uint8_t *data, size_t len, char *name_buf, size_t name_buf_len) {
  if (name_buf == NULL || name_buf_len == 0) {
    return;
  }

  name_buf[0] = '\0';

  if (data == NULL || len < 2) {
    return;
  }

  // BLE advertisement field types for device name
  const uint8_t BLE_AD_TYPE_NAME_COMPLETE = 0x09;
  const uint8_t BLE_AD_TYPE_NAME_SHORT = 0x08;

  size_t index = 0;
  while (index < len) {
    uint8_t field_len = data[index];
    if (field_len == 0) {
      break;
    }
    if (index + field_len >= len) {
      break;
    }
    uint8_t field_type = data[index + 1];
    if (field_type == BLE_AD_TYPE_NAME_COMPLETE || field_type == BLE_AD_TYPE_NAME_SHORT) {
      size_t name_len = field_len - 1;
      if (name_len >= name_buf_len) {
        name_len = name_buf_len - 1;
      }
      memcpy(name_buf, &data[index + 2], name_len);
      name_buf[name_len] = '\0';
      return;
    }
    index += field_len + 1;
  }
}