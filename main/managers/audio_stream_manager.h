#ifndef AUDIO_STREAM_MANAGER_H
#define AUDIO_STREAM_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Audio stream state */
typedef enum {
    AUDIO_STREAM_STATE_IDLE = 0,
    AUDIO_STREAM_STATE_PLAYING,
    AUDIO_STREAM_STATE_PAUSED,
    AUDIO_STREAM_STATE_STOPPED
} audio_stream_state_t;

/**
 * @brief Initialize the audio stream manager.
 *
 * Scans the configured audio directory for MP3 files.
 *
 * @return ESP_OK on success
 */
esp_err_t audio_stream_manager_init(void);

/**
 * @brief Deinitialize the audio stream manager.
 */
void audio_stream_manager_deinit(void);

/**
 * @brief Get the current stream state.
 */
audio_stream_state_t audio_stream_manager_get_state(void);

/**
 * @brief Get the number of MP3 files found.
 */
int audio_stream_manager_get_file_count(void);

/**
 * @brief Get the filename at the given index.
 *
 * @param index File index
 * @return Filename string (internal pointer, do not free), or NULL
 */
const char *audio_stream_manager_get_filename(int index);

/**
 * @brief Get the index of the currently playing file.
 */
int audio_stream_manager_get_current_index(void);

/**
 * @brief Start playing the file at the given index.
 *
 * @param index File index (0-based)
 * @return ESP_OK on success
 */
esp_err_t audio_stream_manager_play(int index);

/**
 * @brief Pause playback.
 */
esp_err_t audio_stream_manager_pause(void);

/**
 * @brief Resume playback.
 */
esp_err_t audio_stream_manager_resume(void);

/**
 * @brief Stop playback.
 */
esp_err_t audio_stream_manager_stop(void);

/**
 * @brief Play the next file in the list.
 */
esp_err_t audio_stream_manager_next(void);

/**
 * @brief Play the previous file in the list.
 */
esp_err_t audio_stream_manager_prev(void);

/**
 * @brief Check if the audio stream manager is initialized.
 */
bool audio_stream_manager_is_initialized(void);

/**
 * @brief Whether the last audio directory scan could access SD storage.
 */
bool audio_stream_manager_sd_available(void);

#ifdef __cplusplus
}
#endif

#endif // AUDIO_STREAM_MANAGER_H
