#ifndef AUDIO_RECEIVER_MANAGER_H
#define AUDIO_RECEIVER_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the audio receiver manager on the S3 side.
 *
 * Sets up the GhostLink stream handler and decoder pipeline.
 *
 * @return ESP_OK on success
 */
esp_err_t audio_receiver_manager_init(void);

/**
 * @brief Deinitialize the audio receiver.
 */
void audio_receiver_manager_deinit(void);

/**
 * @brief Check if the receiver is initialized.
 */
bool audio_receiver_manager_is_initialized(void);

/**
 * @brief Set the DAC sample rate based on decoded MP3 info.
 *
 * @param sample_rate Detected sample rate in Hz
 * @return ESP_OK on success
 */
esp_err_t audio_receiver_manager_set_sample_rate(uint32_t sample_rate);

#ifdef __cplusplus
}
#endif

#endif // AUDIO_RECEIVER_MANAGER_H
