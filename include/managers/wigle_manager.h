#ifndef WIGLE_MANAGER_H
#define WIGLE_MANAGER_H

#include "esp_err.h"

/**
 * Set Wigle API credentials (format: "APIName:APIToken" from wigle.net/account)
 * Persists to NVS.
 */
void wigle_set_api_key(const char *key);

/**
 * Get current Wigle API key (empty if not set).
 */
const char *wigle_get_api_key(void);

/**
 * Upload Wigle-compatible CSV files from SD card to Wigle.net.
 * Processes .wigle_queue (paths added when GPS logger closes a CSV); if queue
 * is missing, scans /mnt/ghostesp/gps/ for .csv and enqueues valid Wigle CSVs.
 * Requires: wigle API key set, device connected to WiFi (STA), SD card mounted.
 * Returns: ESP_OK if all eligible files uploaded, or first error encountered.
 */
esp_err_t wigle_upload_all(void);

/** List stored uploaded CSV entries. */
void wigle_uploaded_list(void);

/** Add file path to upload queue (call when CSV file is created). */
void wigle_queue_add(const char *filepath);

/** Spawn task to run wigle_upload_all in background. CLI returns immediately. */
void wigle_upload_all_async(void);

#endif /* WIGLE_MANAGER_H */
