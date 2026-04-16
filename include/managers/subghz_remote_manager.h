#ifndef SUBGHZ_REMOTE_MANAGER_H
#define SUBGHZ_REMOTE_MANAGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "managers/subghz_decoders.h"

#define SUBGHZ_SCANNER_CHANNEL_COUNT 64
#define SUBGHZ_STREAM_VERSION 1
#define SUBGHZ_SNAPSHOT_NAME_MAX 64
#define SUBGHZ_RAW_MAX_DURATIONS 512

bool subghz_remote_manager_start(bool stream_to_peer);
void subghz_remote_manager_stop(void);
void subghz_remote_manager_set_paused(bool paused);
bool subghz_remote_manager_is_running(void);
bool subghz_remote_manager_is_paused(void);
const char *subghz_remote_manager_get_last_error(void);
bool subghz_remote_manager_get_levels(uint8_t *out_levels, size_t max_levels, uint8_t *out_cursor);
bool subghz_remote_manager_take_raw_capture(int32_t *out_durations, size_t max_durations, size_t *out_count);
bool subghz_remote_manager_take_decode_result(subghz_decoded_signal_t *out_result);
bool subghz_remote_manager_transmit_raw(const int32_t *durations, size_t count);
void subghz_remote_manager_register_stream_handler(void);
void subghz_remote_manager_set_raw_capture_enabled(bool enabled);
bool subghz_remote_manager_capture_snapshot(const char *name_hint);
bool subghz_remote_manager_save_snapshot(const char *name_hint, char *out_path, size_t out_path_len);
bool subghz_remote_manager_load_snapshot(const char *name_or_path);
int subghz_remote_manager_list_snapshots(char names[][SUBGHZ_SNAPSHOT_NAME_MAX], int max_names);
const char *subghz_remote_manager_get_active_snapshot_name(void);

#endif
