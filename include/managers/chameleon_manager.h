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

#ifdef __cplusplus
}
#endif

#endif // CHAMELEON_MANAGER_H
