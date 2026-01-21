// serial_manager.h

#ifndef UTILS_H
#define UTILS_H

#include <esp_types.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

const char *wrap_message(const char *message, const char *file, int line);

void scale_grb_by_brightness(uint8_t *g, uint8_t *r, uint8_t *b, float brightness);

void scale_grb_by_neopixel_brightness(uint8_t *g, uint8_t *r, uint8_t *b, float base_brightness, uint8_t max_brightness_percent);

bool is_in_task_context(void);

void url_decode(char *decoded, const char *encoded);

int get_query_param_value(const char *query, const char *key, char *value, size_t value_size);

int get_next_pcap_file_index(const char *base_name);

int get_next_csv_file_index(const char *base_name);

int get_next_file_index(const char *dir_path, const char *base_name, const char *extension);

void log_heap_status(const char *tag, const char *event);

void format_mac_address(const uint8_t *mac, char *buffer, size_t buffer_len, bool uppercase);

bool str_copy_upper(char *dst, size_t dst_size, const char *src);

#define WRAP_MESSAGE(msg) wrap_message(msg, __FILE__, __LINE__)

#endif // SERIAL_MANAGER_H