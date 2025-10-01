#pragma once

#include "sdkconfig.h"
#if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6)

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Start IEEE 802.15.4 (Zigbee) packet capture on the given channel (11-26).
// If channel == 0, enables channel hopping across 11-26.
esp_err_t zigbee_manager_start_capture(uint8_t channel);

// Stop capture and deinitialize radio.
void zigbee_manager_stop_capture(void);

// Query capture state
bool zigbee_manager_is_capturing(void);

// Enable or disable Zigbee-only filtering (best-effort heuristic to drop Thread/6LoWPAN frames)
void zigbee_manager_set_filter_zigbee_only(bool enable);

#ifdef __cplusplus
}
#endif

#endif // C5/C6 only
