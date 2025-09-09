/**
 * @file chameleon_manager.h
 * @brief Manager for Chameleon Ultra BLE communication
 */

#ifndef CHAMELEON_MANAGER_H
#define CHAMELEON_MANAGER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the Chameleon manager
 */
void chameleon_manager_init(void);

/**
 * @brief Connect to a Chameleon Ultra device
 * @param timeout_seconds Timeout in seconds for the scan and connection
 * @return true if connected successfully, false otherwise
 */
bool chameleon_manager_connect(uint32_t timeout_seconds);

/**
 * @brief Disconnect from the Chameleon Ultra device
 */
void chameleon_manager_disconnect(void);

/**
 * @brief Check if connected to a Chameleon Ultra device
 * @return true if connected, false otherwise
 */
bool chameleon_manager_is_connected(void);

/**
 * @brief Scan for HF tags using the connected Chameleon Ultra
 * @return true if scan was successful, false otherwise
 */
bool chameleon_manager_scan_hf(void);

/**
 * @brief Scan for LF tags using the connected Chameleon Ultra
 * @return true if scan was successful, false otherwise
 */
bool chameleon_manager_scan_lf(void);

/**
 * @brief Get battery information from the Chameleon Ultra
 * @return true if information was retrieved successfully, false otherwise
 */
bool chameleon_manager_get_battery_info(void);

/**
 * @brief Set the Chameleon Ultra to reader mode
 * @return true if mode was set successfully, false otherwise
 */
bool chameleon_manager_set_reader_mode(void);

/**
 * @brief Set the Chameleon Ultra to emulator mode
 * @return true if mode was set successfully, false otherwise
 */
bool chameleon_manager_set_emulator_mode(void);

/**
 * @brief Get firmware version information
 * @return true if version was retrieved successfully, false otherwise
 */
bool chameleon_manager_get_firmware_version(void);

/**
 * @brief Get git version information
 * @return true if version was retrieved successfully, false otherwise
 */
bool chameleon_manager_get_git_version(void);

/**
 * @brief Get device model information
 * @return true if model was retrieved successfully, false otherwise
 */
bool chameleon_manager_get_device_model(void);

/**
 * @brief Get device chip ID
 * @return true if chip ID was retrieved successfully, false otherwise
 */
bool chameleon_manager_get_chip_id(void);

/**
 * @brief Get current device mode
 * @return true if mode was retrieved successfully, false otherwise
 */
bool chameleon_manager_get_device_mode(void);

/**
 * @brief Get active slot number
 * @return true if slot was retrieved successfully, false otherwise
 */
bool chameleon_manager_get_active_slot(void);

/**
 * @brief Set active slot number
 * @param slot Slot number (0-7)
 * @return true if slot was set successfully, false otherwise
 */
bool chameleon_manager_set_active_slot(uint8_t slot);

/**
 * @brief Get slot information
 * @param slot Slot number (0-7)
 * @return true if slot info was retrieved successfully, false otherwise
 */
bool chameleon_manager_get_slot_info(uint8_t slot);

/**
 * @brief Detect MIFARE Classic support on detected tag
 * @return true if detection was successful, false otherwise
 */
bool chameleon_manager_mf1_detect_support(void);

/**
 * @brief Detect MIFARE Classic PRNG type
 * @return true if detection was successful, false otherwise
 */
bool chameleon_manager_mf1_detect_prng(void);

/**
 * @brief Scan for HID Prox tags
 * @return true if scan was successful, false otherwise
 */
bool chameleon_manager_scan_hidprox(void);

/**
 * @brief Save last HF scan data to SD card
 * @param filename Custom filename (optional, can be NULL for auto-naming)
 * @return true if data was saved successfully, false otherwise
 */
bool chameleon_manager_save_last_hf_scan(const char* filename);

/**
 * @brief Save last LF scan data to SD card
 * @param filename Custom filename (optional, can be NULL for auto-naming)
 * @return true if data was saved successfully, false otherwise
 */
bool chameleon_manager_save_last_lf_scan(const char* filename);

/**
 * @brief Read full HF card data (multiple sectors/pages)
 * @return true if card was read successfully, false otherwise
 */
bool chameleon_manager_read_hf_card(void);

/**
 * @brief Read full LF card data
 * @return true if card was read successfully, false otherwise
 */
bool chameleon_manager_read_lf_card(void);

/**
 * @brief Save last full card dump to SD card
 * @param filename Custom filename (optional, can be NULL for auto-naming)
 * @return true if data was saved successfully, false otherwise
 */
bool chameleon_manager_save_card_dump(const char* filename);


/**
 * @brief Detect and identify NTAG card type and version
 * @return true if NTAG card detected and identified, false otherwise
 */
bool chameleon_manager_detect_ntag(void);

/**
 * @brief Read complete NTAG card data (all pages)
 * @return true if successful, false otherwise
 */
bool chameleon_manager_read_ntag_card(void);

/**
 * @brief Save NTAG dump data to SD card
 * @param filename Custom filename (optional, can be NULL for auto-naming)
 * @return true if data was saved successfully, false otherwise
 */
bool chameleon_manager_save_ntag_dump(const char* filename);

/**
 * @brief Attempt NTAG password authentication
 * @param password 4-byte password to try
 * @return true if authentication succeeded
 */
bool chameleon_manager_ntag_authenticate(uint32_t password);

// Debug and testing functions
bool chameleon_manager_test_auth(uint8_t block, uint8_t key_type, const char* key_hex);
bool chameleon_manager_test_both_keys(uint8_t block, const char* key_hex);
bool chameleon_manager_enable_mfkey32_mode(void);
bool chameleon_manager_collect_nonces(void);

#ifdef __cplusplus
}
#endif

#endif // CHAMELEON_MANAGER_H
