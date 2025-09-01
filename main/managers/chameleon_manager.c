/**
 * @file chameleon_manager.c
 * @brief Manager for Chameleon Ultra BLE communication using BLE manager
 */

#include "managers/chameleon_manager.h"
#include "managers/ble_manager.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/ble_gatt.h"
#include "host/ble_gap.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "managers/views/terminal_screen.h"
#include "managers/sd_card_manager.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <inttypes.h>
#include "esp_heap_caps.h"

static const char *TAG = "chameleon_manager";

// Global state
static bool g_is_initialized = false;
static bool g_device_found = false;
static bool g_is_connected = false;
static bool g_scanning = false;
static struct ble_gap_disc_desc g_discovered_device;
static uint16_t g_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t g_tx_char_handle = 0;
static uint16_t g_rx_char_handle = 0;
static SemaphoreHandle_t g_scan_sem = NULL;
static SemaphoreHandle_t g_connect_sem = NULL;
static SemaphoreHandle_t g_response_sem = NULL;

// Chameleon Ultra command constants (from official protocol documentation)
// Basic device commands (1000-1099)
#define CMD_GET_APP_VERSION     1000  // 0x03E8 - Get firmware version
#define CMD_CHANGE_DEVICE_MODE  1001  // 0x03E9 - Change device mode
#define CMD_GET_DEVICE_MODE     1002  // 0x03EA - Get current device mode
#define CMD_SET_ACTIVE_SLOT     1003  // 0x03EB - Set active slot
#define CMD_GET_DEVICE_CHIP_ID  1011  // 0x03F3 - Get device chip ID
#define CMD_GET_DEVICE_ADDRESS  1012  // 0x03F4 - Get device BLE address
#define CMD_GET_GIT_VERSION     1017  // 0x03F9 - Get git version info
#define CMD_GET_ACTIVE_SLOT     1018  // 0x03FA - Get active slot
#define CMD_GET_SLOT_INFO       1019  // 0x03FB - Get slot information
#define CMD_GET_BATTERY_INFO    1025  // 0x0401 - Get battery information
#define CMD_GET_DEVICE_MODEL    1033  // 0x0409 - Get device model
#define CMD_GET_DEVICE_SETTINGS 1034  // 0x040A - Get device settings
#define CMD_GET_DEVICE_CAPABILITIES 1035 // 0x040B - Get device capabilities

// HF commands (2000-2999)
#define CMD_HF14A_SCAN          2000  // 0x07D0 - Official HF14A_SCAN command
#define CMD_MF1_DETECT_SUPPORT  2001  // 0x07D1 - Detect MIFARE Classic support
#define CMD_MF1_DETECT_PRNG     2002  // 0x07D2 - Detect PRNG type
#define CMD_MF1_NESTED_ACQUIRE  2006  // 0x07D6 - Nested attack
#define CMD_MF1_DARKSIDE_ACQUIRE 2004 // 0x07D4 - Darkside attack
#define CMD_MF1_AUTH_ONE_KEY_BLOCK  2007  // 0x07D7 - Authenticate with key for one block
#define CMD_MF1_READ_ONE_BLOCK      2008  // 0x07D8 - Read MIFARE Classic block SUCCESS: OFFICIAL
#define CMD_MF1_WRITE_ONE_BLOCK     2009  // 0x07D9 - Write MIFARE Classic block SUCCESS: OFFICIAL  
#define CMD_HF14A_RAW           2010  // 0x07DA - Send raw HF command

// LF commands (3000-3999)
#define CMD_EM410X_SCAN         3000  // 0x0BB8 - Official EM410X_SCAN command
#define CMD_HIDPROX_SCAN        3002  // 0x0BBA - HID Prox scan

// Emulation commands (4000-4999) - FOR PROPER RF-BASED KEY RECOVERY
#define CMD_MF1_SET_DETECTION_ENABLE 4004  // 0x0FA4 - Enable MFKey32 detection/logging
#define CMD_MF1_GET_DETECTION_COUNT  4005  // 0x0FA5 - Get detection count
#define CMD_MF1_GET_DETECTION_LOG    4006  // 0x0FA6 - Get detection log (nonces)
#define CMD_MF1_GET_DETECTION_ENABLE 4007  // 0x0FA7 - Get detection enable status
#define CMD_HF14A_SET_ANTI_COLL_DATA 4001  // 0x0FA1 - Set anticollision data (UID, etc.)

// NTAG specific commands (using HF14A_RAW for NTAG operations)
#define NTAG_READ_CMD           0x30  // NTAG READ command
#define NTAG_WRITE_CMD          0xA2  // NTAG WRITE command  
#define NTAG_PWD_AUTH_CMD       0x1B  // NTAG password authentication
#define NTAG_GET_VERSION_CMD    0x60  // NTAG GET_VERSION command

// NTAG card memory sizes and page counts
#define NTAG213_TOTAL_PAGES     45   // 180 bytes total
#define NTAG215_TOTAL_PAGES     135  // 540 bytes total  
#define NTAG216_TOTAL_PAGES     231  // 924 bytes total
#define NTAG_PAGE_SIZE          4    // Each page is 4 bytes

// NTAG memory map constants
#define NTAG_HEADER_PAGES       4    // Pages 0-3: Header (UID, etc.)
#define NTAG_USER_START_PAGE    4    // User data starts at page 4
#define NTAG_CC_PAGE            3    // Capability Container at page 3

// Common NTAG default passwords for authentication attempts
static const uint32_t ntag_default_passwords[] = {
    0x00000000,  // All zeros (most common)
    0xFFFFFFFF,  // All ones
    0x12345678,  // Common test password
    0xABCDEF12,  // Another test password
    0x11223344,  // Pattern password
    0x44332211,  // Reverse pattern
};

// Mode constants
#define HW_MODE_READER          0x01
#define HW_MODE_EMULATOR        0x00

// Status codes
#define STATUS_SUCCESS          0x68
#define STATUS_HF_TAG_OK        0x00
#define STATUS_HF_TAG_NO        0x01
#define STATUS_LF_TAG_OK        0x00
#define STATUS_LF_TAG_NO        0x01

// Last scan data storage
typedef struct {
    bool valid;
    uint8_t uid[20];  // Max UID size
    uint8_t uid_size;
    char tag_type[32];
    time_t timestamp;
} last_scan_data_t;

// Full card dump storage
#define MAX_CARD_BLOCKS 256
#define BLOCK_SIZE 16

typedef struct {
    bool valid;
    uint8_t uid[20];
    uint8_t uid_size;
    char tag_type[32];
    time_t timestamp;
    
    // Card data
    uint8_t blocks[MAX_CARD_BLOCKS][BLOCK_SIZE];
    bool block_valid[MAX_CARD_BLOCKS];
    uint16_t total_blocks_read;
    uint16_t card_size_blocks;
    
    // MIFARE Classic specific
    uint16_t atqa;
    uint8_t sak;
} card_dump_data_t;

// NTAG-specific dump structure
typedef struct {
    bool valid;
    uint8_t uid[10];  // NTAG UIDs can be up to 10 bytes
    uint8_t uid_size;
    char card_type[32];  // NTAG213, NTAG215, NTAG216
    time_t timestamp;
    
    // NTAG memory structure
    uint8_t pages[NTAG216_TOTAL_PAGES][NTAG_PAGE_SIZE];  // Use largest possible size
    bool page_valid[NTAG216_TOTAL_PAGES];
    uint16_t total_pages;
    uint16_t readable_pages;
    uint16_t protected_pages;
    
    // NTAG-specific fields
    uint8_t version_data[8];  // GET_VERSION response
    bool version_valid;
    bool password_protected;
    uint32_t password;        // If password authentication succeeded
    bool password_found;
    
    // Memory map information
    uint16_t user_memory_start;  // Usually page 4
    uint16_t user_memory_end;    // Varies by NTAG type
    uint16_t config_pages_start; // Configuration area
    uint16_t lock_bytes_page;    // Lock bytes location
    
    // NDEF information (if present)
    bool ndef_present;
    uint16_t ndef_size;
    uint16_t ndef_start_page;
} ntag_dump_data_t;

static last_scan_data_t g_last_hf_scan = {0};
static last_scan_data_t g_last_lf_scan = {0};
static card_dump_data_t g_last_card_dump = {0};
static ntag_dump_data_t g_last_ntag_dump = {0};

// Comprehensive MIFARE Classic dictionary (based on UberGuidoZ Flipper collection)
// Source: https://github.com/UberGuidoZ/Flipper/blob/main/NFC/mf_classic_dict/mf_classic_dict.nfc
static const uint8_t default_keys[][6] = {
    // Factory and transport defaults
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, // Factory default (FFFFFFFFFFFF)
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, // All zeros (000000000000)
    {0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5}, // Common transport key (A0A1A2A3A4A5)
    {0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5}, // Common transport key (B0B1B2B3B4B5)
    {0xD3, 0xF7, 0xD3, 0xF7, 0xD3, 0xF7}, // NFC default (D3F7D3F7D3F7)
    
    // Corporate and System Keys
    {0x4D, 0x3A, 0x99, 0xC3, 0x51, 0xDD}, // HID Corporate (4D3A99C351DD)
    {0x1A, 0x98, 0x2C, 0x7E, 0x45, 0x9A}, // Infineon default (1A982C7E459A)
    {0x8F, 0xD0, 0xA4, 0xF2, 0x56, 0xE9}, // BW custom key (8FD0A4F256E9)
    {0x50, 0x9F, 0x13, 0x1C, 0x86, 0x3C}, // Bluebird key (509F131C863C)
    
    // Hotel and Access Control Keys
    {0x71, 0x4C, 0x5C, 0x88, 0x6E, 0x97}, // Hotel key (714C5C886E97)
    {0x58, 0x7E, 0xE5, 0xF9, 0x35, 0x0F}, // Hotel key (587EE5F9350F)
    {0xA2, 0x2A, 0xE1, 0x29, 0xC0, 0x13}, // Hotel key (A22AE129C013)
    {0xF8, 0x26, 0x37, 0x16, 0x87, 0x2B}, // Hotel key (F82637168872B)
    {0x67, 0x2A, 0xC6, 0x3F, 0x1F, 0xB1}, // Hotel key (672AC63F1FB1)
    
    // Transport and Ticketing
    {0x48, 0x4D, 0x41, 0x42, 0x4C, 0x31}, // HMAB key (484D41424C31) 
    {0x0E, 0x8F, 0x15, 0x61, 0x54, 0x83}, // Transport key (0E8F15615483)
    {0x9E, 0xA3, 0x25, 0x73, 0xBD, 0xF2}, // Transport key (9EA32573BDF2)
    {0x33, 0x40, 0x52, 0x45, 0x4C, 0x32}, // Transport key (33405245452)
    
    // Common Test and Development Keys
    {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF}, // Test key (AABBCCDDEEFF)
    {0x12, 0x34, 0x56, 0x78, 0x90, 0xAB}, // Test key (1234567890AB)
    {0xAB, 0xCD, 0xEF, 0x12, 0x34, 0x56}, // Test key (ABCDEF123456)
    {0xFE, 0xDC, 0xBA, 0x98, 0x76, 0x54}, // Test key (FEDCBA987654)
    {0x11, 0x22, 0x33, 0x44, 0x55, 0x66}, // Test key (112233445566)
    {0x66, 0x55, 0x44, 0x33, 0x22, 0x11}, // Test key (665544332211)
    
    // Academic and Research Keys
    {0x48, 0x45, 0x4C, 0x4C, 0x4F, 0x21}, // HELLO! (48454C4C4F21)
    {0x54, 0x72, 0x61, 0x63, 0x6B, 0x32}, // Track2 (547261636B32)
    {0x4B, 0x45, 0x59, 0x41, 0x55, 0x54}, // KEYAUT (4B4559415554)
    {0x50, 0x41, 0x53, 0x53, 0x30, 0x31}, // PASS01 (504153533031)
    
    // European Transport Keys
    {0x16, 0x24, 0x36, 0x48, 0xA6, 0x94}, // European transport (16243648A694)
    {0x26, 0x94, 0x16, 0x48, 0x36, 0xA6}, // European transport (26941648336A6)
    {0x46, 0x93, 0x5A, 0x2C, 0xE0, 0x15}, // European transport (46935A2CE015)
    {0x36, 0xA4, 0x26, 0x94, 0x16, 0x48}, // European transport (36A4269416448)
    
    // Laundry and Campus Cards
    {0x49, 0x45, 0x4D, 0x4B, 0x41, 0x45}, // Campus card (49454D4B4145)
    {0x50, 0x56, 0x4C, 0x31, 0x32, 0x33}, // Campus card (50564C313233)
    {0x44, 0x52, 0x45, 0x41, 0x4D, 0x53}, // Campus card (445245414D53)
    
    // Magnetic Card Emulation Keys
    {0x51, 0x4B, 0x33, 0x56, 0x78, 0x2A}, // Mag stripe (514B33567878A)
    {0x75, 0xCD, 0xB2, 0x62, 0x32, 0x9A}, // Mag stripe (75CDB262329A)
    {0x32, 0xAC, 0x3B, 0x90, 0xF7, 0xE1}, // Mag stripe (32AC3B90F7E1)
    
    // Door and Building Access
    {0x42, 0x52, 0x4F, 0x4B, 0x45, 0x4E}, // BROKEN (42524F4B454E)
    {0x44, 0x4F, 0x4F, 0x52, 0x30, 0x31}, // DOOR01 (444F4F52330301)
    {0x41, 0x43, 0x43, 0x45, 0x53, 0x53}, // ACCESS (414343455353)
    {0x42, 0x41, 0x44, 0x43, 0x4F, 0x44}, // BADCOD (424144434F44)
    
    // Payment and Financial
    {0x8A, 0x19, 0xB0, 0x1E, 0x8A, 0x6A}, // Payment system (8A19B01E8A6A)
    {0x47, 0x52, 0x4F, 0x55, 0x50, 0x45}, // Payment system (47524F555045)
    {0x76, 0x6F, 0x72, 0x62, 0x69, 0x73}, // Payment system (766F726269773)
    
    // Additional Commonly Found Keys
    {0xA0, 0xB0, 0xC0, 0xD0, 0xE0, 0xF0}, // Sequence key (A0B0C0D0E0F0)
    {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB}, // Sequence key (0123456789AB)
    {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC}, // Sequence key (123456789ABC)
    {0xC0, 0xDE, 0xDE, 0xAD, 0xBE, 0xEF}, // Debug key (C0DEDEDADBEEF)
    {0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED}, // Debug key (DEADBEEFFEED)
    {0xBA, 0xDA, 0x55, 0xC0, 0xFF, 0xEE}, // Debug key (BADA55C0FFEE)
    
    // Wildcard and Pattern Keys
    {0x12, 0x12, 0x12, 0x12, 0x12, 0x12}, // Pattern key (121212121212)
    {0x34, 0x34, 0x34, 0x34, 0x34, 0x34}, // Pattern key (343434343434)
    {0x56, 0x56, 0x56, 0x56, 0x56, 0x56}, // Pattern key (565656565656)
    {0x78, 0x78, 0x78, 0x78, 0x78, 0x78}, // Pattern key (787878787878)
    {0x9A, 0x9A, 0x9A, 0x9A, 0x9A, 0x9A}, // Pattern key (9A9A9A9A9A9A)
    {0xBC, 0xBC, 0xBC, 0xBC, 0xBC, 0xBC}, // Pattern key (BCBCBCBCBCBC)
};

#define NUM_DEFAULT_KEYS (sizeof(default_keys) / sizeof(default_keys[0]))

// Key types for authentication
#define MF_KEY_A 0x60
#define MF_KEY_B 0x61

// Enhanced card dump with recovered keys
typedef struct {
    uint8_t key[6];
    bool valid;
    bool recovered_by_darkside;
    bool recovered_by_nested;
    bool attack_data_collected; // True if we have attack data but not the actual key yet
} sector_key_t;

typedef struct {
    sector_key_t key_a;
    sector_key_t key_b;
    bool auth_success_a;
    bool auth_success_b;
} sector_auth_t;

// Darkside attack structures
typedef struct {
    uint8_t uid[4];
    uint32_t nt1;
    uint32_t nt2;
    uint32_t nr;
    uint32_t ar;
    uint8_t block;
    uint8_t key_type;
    bool valid;
} darkside_data_t;

// Nested attack structures
typedef struct {
    uint8_t uid[4];
    uint8_t known_key[6];
    uint8_t known_block;
    uint8_t known_key_type;
    uint8_t target_block;
    uint8_t target_key_type;
    uint32_t nt1;
    uint32_t nt2;
    uint32_t nr;
    uint32_t ar;
    bool valid;
    time_t timestamp;
} nested_data_t;

static darkside_data_t g_darkside_data = {0};
static nested_data_t g_nested_data = {0};

// Sector authentication tracking (16 sectors for MIFARE Classic 1K)
#define MAX_SECTORS 16
static sector_auth_t g_sector_auth[MAX_SECTORS] = {0};

// Service and characteristic UUIDs for Chameleon Ultra
// Service: 6E400001-B5A3-F393-E0A9-E50E24DCCA9E
// TX: 6E400002-B5A3-F393-E0A9-E50E24DCCA9E  (write to this)
// RX: 6E400003-B5A3-F393-E0A9-E50E24DCCA9E  (notifications from this)
static const ble_uuid128_t g_service_uuid = BLE_UUID128_INIT(0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0, 0x93, 0xF3, 0xA3, 0xB5, 0x01, 0x00, 0x40, 0x6E);
static const ble_uuid128_t g_tx_char_uuid = BLE_UUID128_INIT(0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0, 0x93, 0xF3, 0xA3, 0xB5, 0x02, 0x00, 0x40, 0x6E);
static const ble_uuid128_t g_rx_char_uuid = BLE_UUID128_INIT(0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0, 0x93, 0xF3, 0xA3, 0xB5, 0x03, 0x00, 0x40, 0x6E);

// Response structure
typedef struct {
    uint16_t command;
    uint8_t status;
    uint8_t data_size;
    uint8_t data[200];
} chameleon_response_t;

static chameleon_response_t g_last_response;
static bool g_response_received = false;

// Forward declarations
static void chameleon_ble_scan_callback(struct ble_gap_event *event, size_t len);
static int chameleon_gap_event_handler(struct ble_gap_event *event, void *arg);
static int chameleon_service_discovery_cb(uint16_t conn_handle, const struct ble_gatt_error *error, const struct ble_gatt_svc *service, void *arg);
static int chameleon_char_discovery_cb(uint16_t conn_handle, const struct ble_gatt_error *error, const struct ble_gatt_chr *chr, void *arg);
static int chameleon_notification_cb(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg);
static bool send_command(uint16_t cmd, uint8_t *data, size_t data_len);
static uint8_t calculate_lrc(const uint8_t *data, size_t length);
static void start_service_discovery(void);

static uint8_t calculate_lrc(const uint8_t *data, size_t length) {
    uint8_t lrc = 0;
    for (size_t i = 0; i < length; i++) {
        lrc += data[i];
    }
    lrc = 0x100 - (lrc & 0xff);
    return lrc;
}

static int chameleon_notification_cb(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    // Check for null pointer
    if (!ctxt || !ctxt->om || !ctxt->om->om_data) {
        ESP_LOGE(TAG, "Invalid notification context");
        return BLE_ATT_ERR_INVALID_HANDLE;
    }
    
    uint16_t data_len = ctxt->om->om_len;
    ESP_LOGI(TAG, "Notification received, length: %d", data_len);
    
    // Minimum Chameleon Ultra response is 10 bytes
    if (data_len >= 10) {
        uint8_t *data = ctxt->om->om_data;
        
        // Parse response: [0x11, 0xef, cmd_hi, cmd_lo, status_hi, status_lo, len_hi, len_lo, header_lrc, data..., data_lrc]
        g_last_response.command = (data[2] << 8) | data[3];
        g_last_response.status = data[5];
        g_last_response.data_size = data[7];
        
        ESP_LOGI(TAG, "Response - Command: 0x%04X, Status: 0x%02X, Data size: %d", 
                g_last_response.command, g_last_response.status, g_last_response.data_size);
        
        // Safely copy data if present and valid
        if (g_last_response.data_size > 0 && 
            g_last_response.data_size <= 200 && 
            data_len >= (9 + g_last_response.data_size)) {
            memcpy(g_last_response.data, data + 9, g_last_response.data_size);
        } else {
            g_last_response.data_size = 0; // Reset if invalid
        }
        
        g_response_received = true;
        
        // Signal response received
        if (g_response_sem) {
            BaseType_t xHigherPriorityTaskWoken = pdFALSE;
            xSemaphoreGiveFromISR(g_response_sem, &xHigherPriorityTaskWoken);
            if (xHigherPriorityTaskWoken) {
                portYIELD_FROM_ISR();
            }
        }
    } else {
        ESP_LOGW(TAG, "Received notification too short: %d bytes", data_len);
    }
    
    return 0;
}

static int chameleon_service_discovery_cb(uint16_t conn_handle, const struct ble_gatt_error *error, const struct ble_gatt_svc *service, void *arg) {
    if (error->status == 0) {
        ESP_LOGI(TAG, "Service discovered");
        // Start characteristic discovery
        int rc = ble_gattc_disc_all_chrs(conn_handle, service->start_handle, service->end_handle, chameleon_char_discovery_cb, NULL);
        if (rc != 0) {
            ESP_LOGE(TAG, "Failed to start characteristic discovery: %d", rc);
        }
    } else {
        ESP_LOGE(TAG, "Service discovery failed: %d", error->status);
    }
    return 0;
}

static int chameleon_char_discovery_cb(uint16_t conn_handle, const struct ble_gatt_error *error, const struct ble_gatt_chr *chr, void *arg) {
    if (error->status == 0 && chr != NULL) {
        // Check if this is TX or RX characteristic
        if (ble_uuid_cmp(&chr->uuid.u, &g_tx_char_uuid.u) == 0) {
            g_tx_char_handle = chr->val_handle;
            ESP_LOGI(TAG, "Found TX characteristic, handle: 0x%04X", g_tx_char_handle);
        } else if (ble_uuid_cmp(&chr->uuid.u, &g_rx_char_uuid.u) == 0) {
            g_rx_char_handle = chr->val_handle;
            ESP_LOGI(TAG, "Found RX characteristic, handle: 0x%04X", g_rx_char_handle);
            
            // Enable notifications by writing to the CCCD (Client Characteristic Configuration Descriptor)
            // CCCD is typically at handle + 1
            uint8_t notify_enable[2] = {0x01, 0x00}; // Enable notifications
            int rc = ble_gattc_write_flat(conn_handle, chr->val_handle + 1, notify_enable, sizeof(notify_enable), NULL, NULL);
            if (rc != 0) {
                ESP_LOGE(TAG, "Failed to enable notifications: %d", rc);
            } else {
                ESP_LOGI(TAG, "Successfully enabled notifications");
            }
        }
    } else if (error->status != 0) {
        ESP_LOGE(TAG, "Characteristic discovery error: %d", error->status);
    }
    return 0;
}

static void start_service_discovery(void) {
    if (g_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }
    
    ESP_LOGI(TAG, "Starting service discovery");
    int rc = ble_gattc_disc_svc_by_uuid(g_conn_handle, &g_service_uuid.u, chameleon_service_discovery_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to start service discovery: %d", rc);
    }
}

static bool send_command(uint16_t cmd, uint8_t *data, size_t data_len) {
    if (g_conn_handle == BLE_HS_CONN_HANDLE_NONE || g_tx_char_handle == 0) {
        ESP_LOGW(TAG, "Not connected or no TX characteristic");
        return false;
    }

    uint8_t payload[200] = {
        0x11, 0xef,
        (cmd >> 8) & 0xFF, cmd & 0xFF,
        0x00, 0x00, (data_len >> 8) & 0xFF, data_len & 0xFF,
        0x00,  // LRC will be calculated
        0x00   // Data LRC will be calculated
    };
    
    // Calculate header LRC
    payload[8] = calculate_lrc(payload + 2, 6);
    
    // Copy data if any
    if (data_len > 0 && data != NULL) {
        memcpy(payload + 9, data, data_len);
    }
    
    // Calculate data LRC
    payload[9 + data_len] = calculate_lrc(payload + 9, data_len);
    
    size_t total_len = 10 + data_len;
    
    ESP_LOGI(TAG, "Sending command 0x%04X with %d bytes data", cmd, (int)data_len);
    
    // Reset response flag
    g_response_received = false;
    
    int rc = ble_gattc_write_no_rsp_flat(g_conn_handle, g_tx_char_handle, payload, total_len);
    return (rc == 0);
}

static void chameleon_ble_scan_callback(struct ble_gap_event *event, size_t len) {
    if (event->type == BLE_GAP_EVENT_DISC) {
        // Check if this is a Chameleon Ultra device
        if (event->disc.event_type == BLE_HCI_ADV_RPT_EVTYPE_ADV_IND ||
            event->disc.event_type == BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP) {
            
            // Look for device name "ChameleonUltra" in advertisement data
            struct ble_hs_adv_fields fields;
            int rc = ble_hs_adv_parse_fields(&fields, event->disc.data, event->disc.length_data);
            if (rc == 0 && fields.name != NULL && fields.name_len >= 13) {
                if (memcmp(fields.name, "ChameleonUltra", 13) == 0) {
                    ESP_LOGI(TAG, "Found Chameleon Ultra device");
                    printf("Found Chameleon Ultra device!\n");
                    TERMINAL_VIEW_ADD_TEXT("Found Chameleon Ultra device!\n");
                    
                    g_discovered_device = event->disc;
                    g_device_found = true;
                    g_scanning = false;
                    
                    // Stop scanning
                    ble_gap_disc_cancel();
                    
                    // Signal scan completion
                    if (g_scan_sem) {
                        xSemaphoreGive(g_scan_sem);
                    }
                    return;
                }
            }
        }
    }
}

static int chameleon_gap_event_handler(struct ble_gap_event *event, void *arg) {
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                ESP_LOGI(TAG, "Connected to Chameleon Ultra");
                printf("Connected to Chameleon Ultra!\n");
                TERMINAL_VIEW_ADD_TEXT("Connected to Chameleon Ultra!\n");
                
                g_conn_handle = event->connect.conn_handle;
                g_is_connected = true;
                
                // Start service discovery to find the correct characteristic handles
                start_service_discovery();
                
                if (g_connect_sem) {
                    xSemaphoreGive(g_connect_sem);
                }
            } else {
                ESP_LOGE(TAG, "Connection failed with status %d", event->connect.status);
                printf("Connection failed\n");
                TERMINAL_VIEW_ADD_TEXT("Connection failed\n");
                g_is_connected = false;
                if (g_connect_sem) {
                    xSemaphoreGive(g_connect_sem);
                }
            }
            break;
            
        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(TAG, "Disconnected from Chameleon Ultra");
            printf("Disconnected from Chameleon Ultra\n");
            TERMINAL_VIEW_ADD_TEXT("Disconnected from Chameleon Ultra\n");
            
            g_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            g_tx_char_handle = 0;
            g_rx_char_handle = 0;
            g_is_connected = false;
            break;
            
        case BLE_GAP_EVENT_NOTIFY_RX:
            ESP_LOGI(TAG, "Notification received on handle 0x%04X", event->notify_rx.attr_handle);
            
            // Check if this is from our RX characteristic
            if (event->notify_rx.attr_handle == g_rx_char_handle) {
                // Process the notification data
                if (event->notify_rx.om) {
                    struct ble_gatt_access_ctxt ctxt = {
                        .op = BLE_GATT_ACCESS_OP_READ_CHR,
                        .om = event->notify_rx.om
                    };
                    chameleon_notification_cb(g_conn_handle, event->notify_rx.attr_handle, &ctxt, NULL);
                }
            }
            break;
            
        default:
            break;
    }
    
    return 0;
}

void chameleon_manager_init(void) {
    if (g_is_initialized) {
        return;
    }
    
    // Check available memory before initialization
    size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "Free heap before Chameleon init: %d bytes", (int)free_heap);
    
    if (free_heap < 20480) { // 20KB minimum
        ESP_LOGE(TAG, "Insufficient memory for Chameleon manager: %d bytes available", (int)free_heap);
        printf("ERROR: Insufficient memory (%d bytes). Need at least 20KB.\n", (int)free_heap);
        TERMINAL_VIEW_ADD_TEXT("ERROR: Insufficient memory for Chameleon manager\n");
        return;
    }
    
    // Create semaphores
    g_scan_sem = xSemaphoreCreateBinary();
    g_connect_sem = xSemaphoreCreateBinary();
    g_response_sem = xSemaphoreCreateBinary();
    
    if (!g_scan_sem || !g_connect_sem || !g_response_sem) {
        ESP_LOGE(TAG, "Failed to create semaphores");
        printf("Failed to initialize Chameleon Ultra manager\n");
        TERMINAL_VIEW_ADD_TEXT("Failed to initialize Chameleon Ultra manager\n");
        return;
    }
    
    // Initialize BLE if not already done
    ble_init();
    
    g_is_initialized = true;
    printf("Chameleon Ultra manager initialized\n");
    TERMINAL_VIEW_ADD_TEXT("Chameleon Ultra manager initialized\n");
    
    free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "Free heap after Chameleon init: %d bytes", (int)free_heap);
}

bool chameleon_manager_connect(uint32_t timeout_seconds) {
    if (!g_is_initialized) {
        chameleon_manager_init();
    }
    
    if (g_is_connected) {
        printf("Already connected to Chameleon Ultra\n");
        TERMINAL_VIEW_ADD_TEXT("Already connected to Chameleon Ultra\n");
        return true;
    }
    
    printf("Searching for Chameleon Ultra device...\n");
    TERMINAL_VIEW_ADD_TEXT("Searching for Chameleon Ultra device...\n");
    
    // Reset state
    g_device_found = false;
    g_scanning = true;
    
    // Register our scan callback
    esp_err_t err = ble_register_handler(chameleon_ble_scan_callback);
    if (err != ESP_OK) {
        printf("Failed to register BLE handler\n");
        TERMINAL_VIEW_ADD_TEXT("Failed to register BLE handler\n");
        return false;
    }
    
    // Start BLE scanning using the existing BLE manager
    ble_start_scanning();
    
    // Wait for device to be found
    if (xSemaphoreTake(g_scan_sem, pdMS_TO_TICKS(timeout_seconds * 1000)) == pdTRUE) {
        if (g_device_found) {
            printf("Chameleon Ultra found! Connecting...\n");
            TERMINAL_VIEW_ADD_TEXT("Chameleon Ultra found! Connecting...\n");
            
            // Unregister scan callback
            ble_unregister_handler(chameleon_ble_scan_callback);
            
            // Try to connect
            int rc = ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &g_discovered_device.addr, 
                                   30000, NULL, chameleon_gap_event_handler, NULL);
            if (rc == 0) {
                // Wait for connection
                if (xSemaphoreTake(g_connect_sem, pdMS_TO_TICKS(10000)) == pdTRUE) {
                    if (g_is_connected) {
                        printf("Successfully connected to Chameleon Ultra\n");
                        TERMINAL_VIEW_ADD_TEXT("Successfully connected to Chameleon Ultra\n");
                        return true;
                    }
                }
            } else {
                ESP_LOGE(TAG, "Failed to start connection, rc=%d", rc);
            }
        }
    }
    
    // Cleanup on failure
    ble_unregister_handler(chameleon_ble_scan_callback);
    ble_stop();
    
    printf("Failed to connect to Chameleon Ultra\n");
    TERMINAL_VIEW_ADD_TEXT("Failed to connect to Chameleon Ultra\n");
    return false;
}

void chameleon_manager_disconnect(void) {
    if (g_is_connected && g_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(g_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        printf("Disconnected from Chameleon Ultra\n");
        TERMINAL_VIEW_ADD_TEXT("Disconnected from Chameleon Ultra\n");
    }
}

bool chameleon_manager_is_connected(void) {
    return g_is_connected;
}

bool chameleon_manager_scan_hf(void) {
    if (!g_is_connected) {
        printf("Not connected to Chameleon Ultra\n");
        TERMINAL_VIEW_ADD_TEXT("Not connected to Chameleon Ultra\n");
        return false;
    }
    
    // First, ensure we're in reader mode for scanning
    printf("Setting reader mode...\n");
    TERMINAL_VIEW_ADD_TEXT("Setting reader mode...\n");
    
    g_response_received = false;
    uint8_t mode_data = HW_MODE_READER;
    if (!send_command(CMD_CHANGE_DEVICE_MODE, &mode_data, 1)) {
        printf("Failed to set reader mode\n");
        TERMINAL_VIEW_ADD_TEXT("Failed to set reader mode\n");
        return false;
    }
    
    // Wait for mode change response
    if (xSemaphoreTake(g_response_sem, pdMS_TO_TICKS(3000)) != pdTRUE || !g_response_received) {
        printf("Mode change timeout\n");
        TERMINAL_VIEW_ADD_TEXT("Mode change timeout\n");
        return false;
    }
    
    if (g_last_response.status != 0x68) { // SUCCESS status
        printf("Failed to set reader mode, status: 0x%02X\n", g_last_response.status);
        TERMINAL_VIEW_ADD_TEXT("Failed to set reader mode, status: 0x%02X\n", g_last_response.status);
        return false;
    }
    
    printf("Reader mode set successfully, scanning for HF tags...\n");
    TERMINAL_VIEW_ADD_TEXT("Reader mode set successfully, scanning for HF tags...\n");
    
    g_response_received = false;
    bool result = send_command(CMD_HF14A_SCAN, NULL, 0);
    if (result) {
        printf("HF scan command sent, waiting for response...\n");
        TERMINAL_VIEW_ADD_TEXT("HF scan command sent, waiting for response...\n");
        
        // Wait for response
        if (xSemaphoreTake(g_response_sem, pdMS_TO_TICKS(10000)) == pdTRUE) {
            if (g_response_received && g_last_response.command == CMD_HF14A_SCAN) {
                if (g_last_response.status == 0x00 || g_last_response.status == 0x68) { // HF_TAG_OK - tag found
                    if (g_last_response.data_size >= 7) {
                        uint8_t uid_size = g_last_response.data[0];
                        printf("HF Tag found!\n");
                        printf("UID (%d bytes): ", uid_size);
                        TERMINAL_VIEW_ADD_TEXT("HF Tag found!\nUID (%d bytes): ", uid_size);
                        
                        for (uint8_t i = 0; i < uid_size && i < 10; i++) {
                            printf("%02X ", g_last_response.data[1 + i]);
                        }
                        printf("\n");
                        
                        uint8_t atqa_lo = g_last_response.data[1 + uid_size];
                        uint8_t atqa_hi = g_last_response.data[2 + uid_size];
                        uint8_t sak = g_last_response.data[3 + uid_size];
                        
                        printf("ATQA: %02X %02X\n", atqa_hi, atqa_lo);
                        printf("SAK: %02X\n", sak);
                        
                        char uid_str[64] = "";
                        for (uint8_t i = 0; i < uid_size && i < 10; i++) {
                            char temp[4];
                            snprintf(temp, sizeof(temp), "%02X ", g_last_response.data[1 + i]);
                            strcat(uid_str, temp);
                        }
                        TERMINAL_VIEW_ADD_TEXT("%s\nATQA: %02X %02X\nSAK: %02X\n", uid_str, atqa_hi, atqa_lo, sak);
                        
                        // Store scan data for saving
                        g_last_hf_scan.valid = true;
                        g_last_hf_scan.uid_size = uid_size;
                        memcpy(g_last_hf_scan.uid, &g_last_response.data[1], uid_size);
                        snprintf(g_last_hf_scan.tag_type, sizeof(g_last_hf_scan.tag_type), 
                                "HF-14A (ATQA:%02X%02X SAK:%02X)", atqa_hi, atqa_lo, sak);
                        g_last_hf_scan.timestamp = time(NULL);
                        
                        return true;
                    }
                } else if (g_last_response.status == 0x01) { // HF_TAG_NO
                    printf("No HF tag found\n");
                    TERMINAL_VIEW_ADD_TEXT("No HF tag found\n");
                } else if (g_last_response.status == 0x66) { // Status we've been seeing
                    printf("HF scan failed: Possibly wrong mode or device state (0x66)\n");
                    TERMINAL_VIEW_ADD_TEXT("HF scan failed: Possibly wrong mode or device state (0x66)\n");
                } else {
                    printf("HF scan failed with status: 0x%02X\n", g_last_response.status);
                    TERMINAL_VIEW_ADD_TEXT("HF scan failed with status: 0x%02X\n", g_last_response.status);
                }
            }
        } else {
            printf("HF scan command timed out\n");
            TERMINAL_VIEW_ADD_TEXT("HF scan command timed out\n");
        }
    } else {
        printf("Failed to send HF scan command\n");
        TERMINAL_VIEW_ADD_TEXT("Failed to send HF scan command\n");
    }
    
    return false;
}

bool chameleon_manager_scan_lf(void) {
    if (!g_is_connected) {
        printf("Not connected to Chameleon Ultra\n");
        TERMINAL_VIEW_ADD_TEXT("Not connected to Chameleon Ultra\n");
        return false;
    }
    
    printf("Scanning for LF tags...\n");
    TERMINAL_VIEW_ADD_TEXT("Scanning for LF tags...\n");
    
    // First, set device to reader mode for LF
    printf("Setting device to reader mode for LF scan...\n");
    TERMINAL_VIEW_ADD_TEXT("Setting device to reader mode for LF scan...\n");
    
    uint8_t mode = HW_MODE_READER;
    bool mode_result = send_command(CMD_CHANGE_DEVICE_MODE, &mode, 1);
    if (!mode_result) {
        printf("Failed to set reader mode\n");
        TERMINAL_VIEW_ADD_TEXT("Failed to set reader mode\n");
        return false;
    }
    
    // Wait for mode change response
    if (xSemaphoreTake(g_response_sem, pdMS_TO_TICKS(5000)) == pdTRUE) {
        if (g_response_received && g_last_response.command == CMD_CHANGE_DEVICE_MODE) {
            if (g_last_response.status != STATUS_SUCCESS) {
                printf("Failed to set reader mode, status: 0x%02X\n", g_last_response.status);
                TERMINAL_VIEW_ADD_TEXT("Failed to set reader mode, status: 0x%02X\n", g_last_response.status);
                return false;
            }
        } else {
            printf("Mode change command failed\n");
            TERMINAL_VIEW_ADD_TEXT("Mode change command failed\n");
            return false;
        }
    } else {
        printf("Mode change command timed out\n");
        TERMINAL_VIEW_ADD_TEXT("Mode change command timed out\n");
        return false;
    }
    
    // Clear response state for the scan command
    g_response_received = false;
    
    bool result = send_command(CMD_EM410X_SCAN, NULL, 0);
    if (result) {
        printf("LF scan command sent, waiting for response...\n");
        TERMINAL_VIEW_ADD_TEXT("LF scan command sent, waiting for response...\n");
        
        // Wait for response
        if (xSemaphoreTake(g_response_sem, pdMS_TO_TICKS(10000)) == pdTRUE) {
            if (g_response_received && g_last_response.command == CMD_EM410X_SCAN) {
                ESP_LOGI(TAG, "LF scan response - Status: 0x%02X, Data size: %d", g_last_response.status, g_last_response.data_size);
                
                if (g_last_response.status == 0x40) { // LF_TAG_OK
                    if (g_last_response.data_size >= 5) {
                        printf("LF Tag found!\n");
                        printf("UID (%d bytes): ", g_last_response.data_size);
                        TERMINAL_VIEW_ADD_TEXT("LF Tag found!\nUID (%d bytes): ", g_last_response.data_size);
                        
                        for (uint8_t i = 0; i < g_last_response.data_size && i < 10; i++) {
                            printf("%02X ", g_last_response.data[i]);
                        }
                        printf("\n");
                        
                        char uid_str[64] = "";
                        for (uint8_t i = 0; i < g_last_response.data_size && i < 10; i++) {
                            char temp[4];
                            snprintf(temp, sizeof(temp), "%02X ", g_last_response.data[i]);
                            strcat(uid_str, temp);
                        }
                        TERMINAL_VIEW_ADD_TEXT("%s\n", uid_str);
                        
                        // Store scan data for saving
                        g_last_lf_scan.valid = true;
                        g_last_lf_scan.uid_size = g_last_response.data_size;
                        memcpy(g_last_lf_scan.uid, g_last_response.data, g_last_response.data_size);
                        snprintf(g_last_lf_scan.tag_type, sizeof(g_last_lf_scan.tag_type), "LF-EM410X");
                        g_last_lf_scan.timestamp = time(NULL);
                        
                        return true;
                    } else {
                        printf("LF Tag found but insufficient data: %d bytes\n", g_last_response.data_size);
                        TERMINAL_VIEW_ADD_TEXT("LF Tag found but insufficient data: %d bytes\n", g_last_response.data_size);
                    }
                } else if (g_last_response.status == 0x41) { // EM410X_TAG_NO_FOUND
                    printf("No EM410X LF tag found\n");
                    TERMINAL_VIEW_ADD_TEXT("No EM410X LF tag found\n");
                } else if (g_last_response.status == STATUS_SUCCESS || g_last_response.status == 0x68) {
                    // Some success status but might be a different tag type
                    printf("LF scan completed successfully but no EM410X tag detected\n");
                    TERMINAL_VIEW_ADD_TEXT("LF scan completed successfully but no EM410X tag detected\n");
                    if (g_last_response.data_size > 0) {
                        printf("Received %d bytes of data: ", g_last_response.data_size);
                        TERMINAL_VIEW_ADD_TEXT("Received %d bytes of data: ", g_last_response.data_size);
                        for (uint8_t i = 0; i < g_last_response.data_size && i < 10; i++) {
                            printf("%02X ", g_last_response.data[i]);
                        }
                        printf("\n");
                        char uid_str[64] = "";
                        for (uint8_t i = 0; i < g_last_response.data_size && i < 10; i++) {
                            char temp[4];
                            snprintf(temp, sizeof(temp), "%02X ", g_last_response.data[i]);
                            strcat(uid_str, temp);
                        }
                        TERMINAL_VIEW_ADD_TEXT("%s\n", uid_str);
                    }
                } else {
                    printf("LF scan failed with status: 0x%02X\n", g_last_response.status);
                    TERMINAL_VIEW_ADD_TEXT("LF scan failed with status: 0x%02X\n", g_last_response.status);
                }
            }
        } else {
            printf("LF scan command timed out\n");
            TERMINAL_VIEW_ADD_TEXT("LF scan command timed out\n");
        }
    } else {
        printf("Failed to send LF scan command\n");
        TERMINAL_VIEW_ADD_TEXT("Failed to send LF scan command\n");
    }
    
    return false;
}

bool chameleon_manager_get_battery_info(void) {
    if (!g_is_connected) {
        printf("Not connected to Chameleon Ultra\n");
        TERMINAL_VIEW_ADD_TEXT("Not connected to Chameleon Ultra\n");
        return false;
    }
    
    printf("Getting battery information...\n");
    TERMINAL_VIEW_ADD_TEXT("Getting battery information...\n");
    
    bool result = send_command(CMD_GET_BATTERY_INFO, NULL, 0);
    if (result) {
        printf("Battery information command sent, waiting for response...\n");
        TERMINAL_VIEW_ADD_TEXT("Battery information command sent, waiting for response...\n");
        
        // Wait for response
        if (xSemaphoreTake(g_response_sem, pdMS_TO_TICKS(5000)) == pdTRUE) {
            if (g_response_received && g_last_response.command == CMD_GET_BATTERY_INFO) {
                if (g_last_response.status == 0x68) { // SUCCESS
                    if (g_last_response.data_size >= 3) {
                        uint16_t voltage = (g_last_response.data[0] << 8) | g_last_response.data[1];
                        uint8_t percentage = g_last_response.data[2];
                        printf("Battery: %dmV (%d%%)\n", voltage, percentage);
                        TERMINAL_VIEW_ADD_TEXT("Battery: %dmV (%d%%)\n", voltage, percentage);
                        return true;
                    } else {
                        printf("Battery data too short: %d bytes\n", g_last_response.data_size);
                        TERMINAL_VIEW_ADD_TEXT("Battery data too short: %d bytes\n", g_last_response.data_size);
                    }
                } else {
                    printf("Battery command failed with status: 0x%02X\n", g_last_response.status);
                    TERMINAL_VIEW_ADD_TEXT("Battery command failed with status: 0x%02X\n", g_last_response.status);
                }
            }
        } else {
            printf("Battery command timed out\n");
            TERMINAL_VIEW_ADD_TEXT("Battery command timed out\n");
        }
    } else {
        printf("Failed to send battery information command\n");
        TERMINAL_VIEW_ADD_TEXT("Failed to send battery information command\n");
    }
    
    return false;
}

bool chameleon_manager_set_reader_mode(void) {
    if (!g_is_connected) {
        printf("Not connected to Chameleon Ultra\n");
        TERMINAL_VIEW_ADD_TEXT("Not connected to Chameleon Ultra\n");
        return false;
    }
    
    printf("Setting Chameleon Ultra to reader mode...\n");
    TERMINAL_VIEW_ADD_TEXT("Setting Chameleon Ultra to reader mode...\n");
    
    uint8_t mode = HW_MODE_READER;
    bool result = send_command(CMD_CHANGE_DEVICE_MODE, &mode, 1);
    if (result) {
        printf("Reader mode command sent\n");
        TERMINAL_VIEW_ADD_TEXT("Reader mode command sent\n");
    } else {
        printf("Failed to send reader mode command\n");
        TERMINAL_VIEW_ADD_TEXT("Failed to send reader mode command\n");
    }
    
    return result;
}

bool chameleon_manager_set_emulator_mode(void) {
    if (!g_is_connected) {
        printf("Not connected to Chameleon Ultra\n");
        TERMINAL_VIEW_ADD_TEXT("Not connected to Chameleon Ultra\n");
        return false;
    }
    
    printf("Setting Chameleon Ultra to emulator mode...\n");
    TERMINAL_VIEW_ADD_TEXT("Setting Chameleon Ultra to emulator mode...\n");
    
    uint8_t mode = HW_MODE_EMULATOR;
    bool result = send_command(CMD_CHANGE_DEVICE_MODE, &mode, 1);
    if (result) {
        printf("Emulator mode command sent\n");
        TERMINAL_VIEW_ADD_TEXT("Emulator mode command sent\n");
    } else {
        printf("Failed to send emulator mode command\n");
        TERMINAL_VIEW_ADD_TEXT("Failed to send emulator mode command\n");
    }
    
    return result;
}

// Device information functions
bool chameleon_manager_get_firmware_version(void) {
    if (!g_is_connected) {
        printf("Not connected to Chameleon Ultra\n");
        TERMINAL_VIEW_ADD_TEXT("Not connected to Chameleon Ultra\n");
        return false;
    }
    
    printf("Getting firmware version...\n");
    TERMINAL_VIEW_ADD_TEXT("Getting firmware version...\n");
    
    g_response_received = false;
    bool result = send_command(CMD_GET_APP_VERSION, NULL, 0);
    if (!result) {
        printf("Failed to send firmware version command\n");
        TERMINAL_VIEW_ADD_TEXT("Failed to send firmware version command\n");
        return false;
    }
    
    // Wait for response
    if (xSemaphoreTake(g_response_sem, pdMS_TO_TICKS(5000)) == pdTRUE) {
        if (g_response_received && g_last_response.command == CMD_GET_APP_VERSION) {
            if (g_last_response.status == STATUS_SUCCESS) {
                if (g_last_response.data_size >= 2) {
                    uint8_t major = g_last_response.data[0];
                    uint8_t minor = g_last_response.data[1];
                    if (g_last_response.data_size >= 3) {
                        uint8_t patch = g_last_response.data[2];
                        printf("Firmware Version: %d.%d.%d\n", major, minor, patch);
                        TERMINAL_VIEW_ADD_TEXT("Firmware Version: %d.%d.%d\n", major, minor, patch);
                    } else {
                        printf("Firmware Version: %d.%d\n", major, minor);
                        TERMINAL_VIEW_ADD_TEXT("Firmware Version: %d.%d\n", major, minor);
                    }
                    return true;
                }
            } else {
                printf("Failed to get firmware version, status: 0x%02X\n", g_last_response.status);
                TERMINAL_VIEW_ADD_TEXT("Failed to get firmware version, status: 0x%02X\n", g_last_response.status);
            }
        }
    } else {
        printf("Firmware version command timed out\n");
        TERMINAL_VIEW_ADD_TEXT("Firmware version command timed out\n");
    }
    
    return false;
}

bool chameleon_manager_get_device_mode(void) {
    if (!g_is_connected) {
        printf("Not connected to Chameleon Ultra\n");
        TERMINAL_VIEW_ADD_TEXT("Not connected to Chameleon Ultra\n");
        return false;
    }
    
    printf("Getting device mode...\n");
    TERMINAL_VIEW_ADD_TEXT("Getting device mode...\n");
    
    // Clear any previous response and reset flag
    memset(&g_last_response, 0, sizeof(g_last_response));
    g_response_received = false;
    
    bool result = send_command(CMD_GET_DEVICE_MODE, NULL, 0);
    if (!result) {
        printf("Failed to send device mode command\n");
        TERMINAL_VIEW_ADD_TEXT("Failed to send device mode command\n");
        return false;
    }
    
    // Wait for response with longer timeout
    if (xSemaphoreTake(g_response_sem, pdMS_TO_TICKS(8000)) == pdTRUE) {
        // Give a small delay for the response to be processed
        vTaskDelay(pdMS_TO_TICKS(100));
        
        ESP_LOGI(TAG, "After semaphore - Response received=%d", g_response_received);
        if (g_response_received) {
            ESP_LOGI(TAG, "Command received=0x%04X, expected=0x%04X", g_last_response.command, CMD_GET_DEVICE_MODE);
        }
        
        if (g_response_received && g_last_response.command == CMD_GET_DEVICE_MODE) {
            ESP_LOGI(TAG, "Status=0x%02X, Data size=%d", g_last_response.status, g_last_response.data_size);
            
            if (g_last_response.data_size >= 1) {
                uint8_t mode = g_last_response.data[0];
                ESP_LOGI(TAG, "Mode byte=0x%02X", mode);
                
                const char* mode_str;
                if (mode == HW_MODE_READER) {
                    mode_str = "Reader";
                } else if (mode == HW_MODE_EMULATOR) {
                    mode_str = "Emulator";
                } else {
                    mode_str = "Unknown";
                }
                
                printf("Device Mode: %s (0x%02X)\n", mode_str, mode);
                TERMINAL_VIEW_ADD_TEXT("Device Mode: %s (0x%02X)\n", mode_str, mode);
                return true;
            } else {
                printf("Failed to get device mode, status: 0x%02X, insufficient data\n", g_last_response.status);
                TERMINAL_VIEW_ADD_TEXT("Failed to get device mode, status: 0x%02X, insufficient data\n", g_last_response.status);
            }
        } else {
            ESP_LOGI(TAG, "Response received=%d, Command match=%d", g_response_received, 
                   g_response_received ? (g_last_response.command == CMD_GET_DEVICE_MODE) : 0);
        }
    } else {
        printf("Device mode command timed out\n");
        TERMINAL_VIEW_ADD_TEXT("Device mode command timed out\n");
    }
    
    return false;
}

bool chameleon_manager_get_active_slot(void) {
    if (!g_is_connected) {
        printf("Not connected to Chameleon Ultra\n");
        TERMINAL_VIEW_ADD_TEXT("Not connected to Chameleon Ultra\n");
        return false;
    }
    
    printf("Getting active slot...\n");
    TERMINAL_VIEW_ADD_TEXT("Getting active slot...\n");
    
    g_response_received = false;
    bool result = send_command(CMD_GET_ACTIVE_SLOT, NULL, 0);
    if (!result) {
        printf("Failed to send active slot command\n");
        TERMINAL_VIEW_ADD_TEXT("Failed to send active slot command\n");
        return false;
    }
    
    // Wait for response
    if (xSemaphoreTake(g_response_sem, pdMS_TO_TICKS(5000)) == pdTRUE) {
        if (g_response_received && g_last_response.command == CMD_GET_ACTIVE_SLOT) {
            if (g_last_response.status == STATUS_SUCCESS && g_last_response.data_size >= 1) {
                uint8_t device_slot = g_last_response.data[0];
                uint8_t user_slot = device_slot + 1; // Convert 0-7 to 1-8
                printf("Active Slot: %d\n", user_slot);
                TERMINAL_VIEW_ADD_TEXT("Active Slot: %d\n", user_slot);
                return true;
            } else {
                printf("Failed to get active slot, status: 0x%02X\n", g_last_response.status);
                TERMINAL_VIEW_ADD_TEXT("Failed to get active slot, status: 0x%02X\n", g_last_response.status);
            }
        }
    } else {
        printf("Active slot command timed out\n");
        TERMINAL_VIEW_ADD_TEXT("Active slot command timed out\n");
    }
    
    return false;
}

bool chameleon_manager_set_active_slot(uint8_t slot) {
    if (!g_is_connected) {
        printf("Not connected to Chameleon Ultra\n");
        TERMINAL_VIEW_ADD_TEXT("Not connected to Chameleon Ultra\n");
        return false;
    }
    
    if (slot > 7) {
        printf("Invalid slot number: %d (must be 0-7)\n", slot);
        TERMINAL_VIEW_ADD_TEXT("Invalid slot number: %d (must be 0-7)\n", slot);
        return false;
    }
    
    uint8_t user_slot = slot + 1; // Convert 0-7 to 1-8 for display
    printf("Setting active slot to %d...\n", user_slot);
    TERMINAL_VIEW_ADD_TEXT("Setting active slot to %d...\n", user_slot);
    
    g_response_received = false;
    bool result = send_command(CMD_SET_ACTIVE_SLOT, &slot, 1);
    if (!result) {
        printf("Failed to send set active slot command\n");
        TERMINAL_VIEW_ADD_TEXT("Failed to send set active slot command\n");
        return false;
    }
    
    // Wait for response
    if (xSemaphoreTake(g_response_sem, pdMS_TO_TICKS(5000)) == pdTRUE) {
        if (g_response_received && g_last_response.command == CMD_SET_ACTIVE_SLOT) {
            if (g_last_response.status == STATUS_SUCCESS) {
                printf("Active slot set to %d successfully\n", user_slot);
                TERMINAL_VIEW_ADD_TEXT("Active slot set to %d successfully\n", user_slot);
                return true;
            } else {
                printf("Failed to set active slot, status: 0x%02X\n", g_last_response.status);
                TERMINAL_VIEW_ADD_TEXT("Failed to set active slot, status: 0x%02X\n", g_last_response.status);
            }
        }
    } else {
        printf("Set active slot command timed out\n");
        TERMINAL_VIEW_ADD_TEXT("Set active slot command timed out\n");
    }
    
    return false;
}

bool chameleon_manager_get_slot_info(uint8_t slot) {
    if (!g_is_connected) {
        printf("Not connected to Chameleon Ultra\n");
        TERMINAL_VIEW_ADD_TEXT("Not connected to Chameleon Ultra\n");
        return false;
    }
    
    if (slot > 7) {
        printf("Invalid slot number: %d (must be 0-7)\n", slot);
        TERMINAL_VIEW_ADD_TEXT("Invalid slot number: %d (must be 0-7)\n", slot);
        return false;
    }
    
    uint8_t user_slot = slot + 1; // Convert 0-7 to 1-8 for display
    printf("Getting slot %d information...\n", user_slot);
    TERMINAL_VIEW_ADD_TEXT("Getting slot %d information...\n", user_slot);
    
    g_response_received = false;
    bool result = send_command(CMD_GET_SLOT_INFO, &slot, 1);
    if (!result) {
        printf("Failed to send slot info command\n");
        TERMINAL_VIEW_ADD_TEXT("Failed to send slot info command\n");
        return false;
    }
    
    // Wait for response
    if (xSemaphoreTake(g_response_sem, pdMS_TO_TICKS(5000)) == pdTRUE) {
        if (g_response_received && g_last_response.command == CMD_GET_SLOT_INFO) {
            if (g_last_response.status == STATUS_SUCCESS && g_last_response.data_size >= 3) {
                uint8_t hf_type = g_last_response.data[0];
                uint8_t lf_type = g_last_response.data[1];
                uint8_t enabled = g_last_response.data[2];
                
                printf("Slot %d Info:\n", user_slot);
                printf("  HF Type: 0x%02X\n", hf_type);
                printf("  LF Type: 0x%02X\n", lf_type);
                printf("  Enabled: %s\n", enabled ? "Yes" : "No");
                
                TERMINAL_VIEW_ADD_TEXT("Slot %d Info:\n", user_slot);
                TERMINAL_VIEW_ADD_TEXT("  HF Type: 0x%02X\n", hf_type);
                TERMINAL_VIEW_ADD_TEXT("  LF Type: 0x%02X\n", lf_type);
                TERMINAL_VIEW_ADD_TEXT("  Enabled: %s\n", enabled ? "Yes" : "No");
                
                return true;
            } else {
                printf("Failed to get slot info, status: 0x%02X\n", g_last_response.status);
                TERMINAL_VIEW_ADD_TEXT("Failed to get slot info, status: 0x%02X\n", g_last_response.status);
            }
        }
    } else {
        printf("Slot info command timed out\n");
        TERMINAL_VIEW_ADD_TEXT("Slot info command timed out\n");
    }
    
    return false;
}

bool chameleon_manager_mf1_detect_support(void) {
    if (!g_is_connected) {
        printf("Not connected to Chameleon Ultra\n");
        TERMINAL_VIEW_ADD_TEXT("Not connected to Chameleon Ultra\n");
        return false;
    }
    
    printf("Detecting MIFARE Classic support...\n");
    TERMINAL_VIEW_ADD_TEXT("Detecting MIFARE Classic support...\n");
    
    // First, run HF scan to detect and establish communication with tag
    printf("Running HF scan first to detect tag...\n");
    TERMINAL_VIEW_ADD_TEXT("Running HF scan first to detect tag...\n");
    
    if (!chameleon_manager_scan_hf()) {
        printf("No HF tag found - MIFARE detection requires a tag to be present\n");
        TERMINAL_VIEW_ADD_TEXT("No HF tag found - MIFARE detection requires a tag to be present\n");
        return false;
    }
    
    // Small delay to ensure scan is complete
    vTaskDelay(pdMS_TO_TICKS(500));
    
    g_response_received = false;
    bool result = send_command(CMD_MF1_DETECT_SUPPORT, NULL, 0);
    if (!result) {
        printf("Failed to send MF1 detect support command\n");
        TERMINAL_VIEW_ADD_TEXT("Failed to send MF1 detect support command\n");
        return false;
    }
    
    // Wait for response
    if (xSemaphoreTake(g_response_sem, pdMS_TO_TICKS(10000)) == pdTRUE) {
        if (g_response_received && g_last_response.command == CMD_MF1_DETECT_SUPPORT) {
            ESP_LOGI(TAG, "MF1 detect response - Status: 0x%02X, Data size: %d", g_last_response.status, g_last_response.data_size);
            
            if ((g_last_response.status == STATUS_SUCCESS || g_last_response.status == 0x00) && g_last_response.data_size >= 1) {
                uint8_t support = g_last_response.data[0];
                const char* support_str = support ? "Supported" : "Not Supported";
                printf("MIFARE Classic Support: %s\n", support_str);
                TERMINAL_VIEW_ADD_TEXT("MIFARE Classic Support: %s\n", support_str);
                return true;
            } else if (g_last_response.status == STATUS_SUCCESS || g_last_response.status == 0x00) {
                // Success status but no data - interpret as supported
                printf("MIFARE Classic Support: Supported (no specific data returned)\n");
                TERMINAL_VIEW_ADD_TEXT("MIFARE Classic Support: Supported (no specific data returned)\n");
                return true;
            } else {
                printf("Failed to detect MF1 support, status: 0x%02X\n", g_last_response.status);
                TERMINAL_VIEW_ADD_TEXT("Failed to detect MF1 support, status: 0x%02X\n", g_last_response.status);
            }
        }
    } else {
        printf("MF1 detect support command timed out\n");
        TERMINAL_VIEW_ADD_TEXT("MF1 detect support command timed out\n");
    }
    
    return false;
}

bool chameleon_manager_mf1_detect_prng(void) {
    if (!g_is_connected) {
        printf("Not connected to Chameleon Ultra\n");
        TERMINAL_VIEW_ADD_TEXT("Not connected to Chameleon Ultra\n");
        return false;
    }
    
    printf("Detecting MIFARE Classic PRNG type...\n");
    TERMINAL_VIEW_ADD_TEXT("Detecting MIFARE Classic PRNG type...\n");
    
    // First, run HF scan to detect and establish communication with tag
    printf("Running HF scan first to detect tag...\n");
    TERMINAL_VIEW_ADD_TEXT("Running HF scan first to detect tag...\n");
    
    if (!chameleon_manager_scan_hf()) {
        printf("No HF tag found - MIFARE PRNG detection requires a tag to be present\n");
        TERMINAL_VIEW_ADD_TEXT("No HF tag found - MIFARE PRNG detection requires a tag to be present\n");
        return false;
    }
    
    // Small delay to ensure scan is complete
    vTaskDelay(pdMS_TO_TICKS(500));
    
    g_response_received = false;
    bool result = send_command(CMD_MF1_DETECT_PRNG, NULL, 0);
    if (!result) {
        printf("Failed to send MF1 detect PRNG command\n");
        TERMINAL_VIEW_ADD_TEXT("Failed to send MF1 detect PRNG command\n");
        return false;
    }
    
    // Wait for response
    if (xSemaphoreTake(g_response_sem, pdMS_TO_TICKS(10000)) == pdTRUE) {
        if (g_response_received && g_last_response.command == CMD_MF1_DETECT_PRNG) {
            ESP_LOGI(TAG, "MF1 PRNG detect response - Status: 0x%02X, Data size: %d", g_last_response.status, g_last_response.data_size);
            
            if ((g_last_response.status == STATUS_SUCCESS || g_last_response.status == 0x00) && g_last_response.data_size >= 1) {
                uint8_t prng_type = g_last_response.data[0];
                const char* prng_str;
                switch (prng_type) {
                    case 0: prng_str = "Static"; break;
                    case 1: prng_str = "Weak"; break;
                    case 2: prng_str = "Hard"; break;
                    default: prng_str = "Unknown"; break;
                }
                printf("MIFARE Classic PRNG Type: %s (0x%02X)\n", prng_str, prng_type);
                TERMINAL_VIEW_ADD_TEXT("MIFARE Classic PRNG Type: %s (0x%02X)\n", prng_str, prng_type);
                return true;
            } else if (g_last_response.status == STATUS_SUCCESS || g_last_response.status == 0x00) {
                // Success status but no data - no MIFARE tag present for PRNG detection
                printf("MIFARE Classic PRNG detection successful, but no MIFARE tag present\n");
                TERMINAL_VIEW_ADD_TEXT("MIFARE Classic PRNG detection successful, but no MIFARE tag present\n");
                return true;
            } else {
                printf("Failed to detect PRNG type, status: 0x%02X\n", g_last_response.status);
                TERMINAL_VIEW_ADD_TEXT("Failed to detect PRNG type, status: 0x%02X\n", g_last_response.status);
            }
        }
    } else {
        printf("MF1 detect PRNG command timed out\n");
        TERMINAL_VIEW_ADD_TEXT("MF1 detect PRNG command timed out\n");
    }
    
    return false;
}

bool chameleon_manager_scan_hidprox(void) {
    if (!g_is_connected) {
        printf("Not connected to Chameleon Ultra\n");
        TERMINAL_VIEW_ADD_TEXT("Not connected to Chameleon Ultra\n");
        return false;
    }
    
    // First, ensure we're in reader mode for scanning
    printf("Setting reader mode for HID Prox scan...\n");
    TERMINAL_VIEW_ADD_TEXT("Setting reader mode for HID Prox scan...\n");
    
    g_response_received = false;
    uint8_t mode_data = HW_MODE_READER;
    if (!send_command(CMD_CHANGE_DEVICE_MODE, &mode_data, 1)) {
        printf("Failed to set reader mode\n");
        TERMINAL_VIEW_ADD_TEXT("Failed to set reader mode\n");
        return false;
    }
    
    // Wait for mode change response
    if (xSemaphoreTake(g_response_sem, pdMS_TO_TICKS(3000)) != pdTRUE || !g_response_received) {
        printf("Mode change timeout\n");
        TERMINAL_VIEW_ADD_TEXT("Mode change timeout\n");
        return false;
    }
    
    if (g_last_response.status != STATUS_SUCCESS) {
        printf("Failed to set reader mode, status: 0x%02X\n", g_last_response.status);
        TERMINAL_VIEW_ADD_TEXT("Failed to set reader mode, status: 0x%02X\n", g_last_response.status);
        return false;
    }
    
    printf("Reader mode set, scanning for HID Prox tags...\n");
    TERMINAL_VIEW_ADD_TEXT("Reader mode set, scanning for HID Prox tags...\n");
    
    g_response_received = false;
    bool result = send_command(CMD_HIDPROX_SCAN, NULL, 0);
    if (!result) {
        printf("Failed to send HID Prox scan command\n");
        TERMINAL_VIEW_ADD_TEXT("Failed to send HID Prox scan command\n");
        return false;
    }
    
    printf("HID Prox scan command sent, waiting for response...\n");
    TERMINAL_VIEW_ADD_TEXT("HID Prox scan command sent, waiting for response...\n");
    
    // Wait for response
    if (xSemaphoreTake(g_response_sem, pdMS_TO_TICKS(10000)) == pdTRUE) {
        if (g_response_received && g_last_response.command == CMD_HIDPROX_SCAN) {
            if (g_last_response.status == STATUS_LF_TAG_OK && g_last_response.data_size >= 5) {
                // Parse HID Prox data: typically 5 bytes
                printf("HID Prox Tag found!\n");
                printf("Tag Data: ");
                TERMINAL_VIEW_ADD_TEXT("HID Prox Tag found!\nTag Data: ");
                
                for (uint8_t i = 0; i < g_last_response.data_size && i < 10; i++) {
                    printf("%02X ", g_last_response.data[i]);
                }
                printf("\n");
                TERMINAL_VIEW_ADD_TEXT("\n");
                
                return true;
            } else if (g_last_response.status == STATUS_LF_TAG_NO) {
                printf("No HID Prox tag found\n");
                TERMINAL_VIEW_ADD_TEXT("No HID Prox tag found\n");
            } else {
                printf("HID Prox scan failed with status: 0x%02X\n", g_last_response.status);
                TERMINAL_VIEW_ADD_TEXT("HID Prox scan failed with status: 0x%02X\n", g_last_response.status);
            }
        }
    } else {
        printf("HID Prox scan command timed out\n");
        TERMINAL_VIEW_ADD_TEXT("HID Prox scan command timed out\n");
    }
    
    return false;
}

/**
 * @brief Try to authenticate to a MIFARE Classic block with default keys
 * @param block Block number to authenticate to
 * @param key_type Key type (MF_KEY_A or MF_KEY_B)
 * @param found_key Pointer to store the successful key (optional)
 * @return true if authentication succeeded, false otherwise
 */
static bool authenticate_mifare_block(uint8_t block, uint8_t key_type, uint8_t* found_key) {
    for (size_t key_idx = 0; key_idx < NUM_DEFAULT_KEYS; key_idx++) {
        // Prepare authentication command: block + key_type + 6-byte key
        uint8_t auth_data[8];
        auth_data[0] = block;
        auth_data[1] = key_type;
        memcpy(&auth_data[2], default_keys[key_idx], 6);
        
        ESP_LOGI("chameleon_manager", "Trying key %zu for block %d: %02X%02X%02X%02X%02X%02X", 
                 key_idx, block,
                 default_keys[key_idx][0], default_keys[key_idx][1], default_keys[key_idx][2],
                 default_keys[key_idx][3], default_keys[key_idx][4], default_keys[key_idx][5]);
        
        g_response_received = false;
        bool result = send_command(CMD_MF1_AUTH_ONE_KEY_BLOCK, auth_data, 8);
        if (!result) {
            ESP_LOGI("chameleon_manager", "Failed to send auth command for block %d", block);
            continue;
        }
        
        // Wait for authentication response
        if (xSemaphoreTake(g_response_sem, pdMS_TO_TICKS(3000)) == pdTRUE) {
            if (g_response_received && g_last_response.command == CMD_MF1_AUTH_ONE_KEY_BLOCK) {
                if (g_last_response.status == STATUS_SUCCESS || g_last_response.status == 0x00) {
                    ESP_LOGI("chameleon_manager", "Authentication successful for block %d with key %zu", block, key_idx);
                    
                    // Store the successful key if requested
                    if (found_key != NULL) {
                        memcpy(found_key, default_keys[key_idx], 6);
                    }
                    
                    return true;
                } else {
                    ESP_LOGI("chameleon_manager", "Authentication failed for block %d with key %zu (status: 0x%02X)", 
                             block, key_idx, g_last_response.status);
                }
            }
        } else {
            ESP_LOGI("chameleon_manager", "Authentication timeout for block %d with key %zu", block, key_idx);
        }
        
        // Small delay between key attempts
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    
    return false;
}

/**
 * @brief Perform MIFARE Classic Nested attack to recover keys
 * @param known_block Block with known key
 * @param known_key_type Known key type (MF_KEY_A or MF_KEY_B)
 * @param known_key The known key (6 bytes)
 * @param target_block Target block to recover key for
 * @param target_key_type Target key type (MF_KEY_A or MF_KEY_B)
 * @return true if attack data was collected successfully
 */
bool chameleon_manager_nested_attack(uint8_t known_block, uint8_t known_key_type, const uint8_t* known_key,
                                    uint8_t target_block, uint8_t target_key_type) {
    if (!g_is_connected) {
        printf("Not connected to Chameleon Ultra\n");
        TERMINAL_VIEW_ADD_TEXT("Not connected to Chameleon Ultra\n");
        return false;
    }
    
    printf("Starting MIFARE Classic Nested attack...\n");
    printf("Known: Block %d, Key %s: %02X%02X%02X%02X%02X%02X\n", 
           known_block, (known_key_type == MF_KEY_A) ? "A" : "B",
           known_key[0], known_key[1], known_key[2], known_key[3], known_key[4], known_key[5]);
    printf("Target: Block %d, Key %s\n", target_block, (target_key_type == MF_KEY_A) ? "A" : "B");
    
    TERMINAL_VIEW_ADD_TEXT("Starting MIFARE Classic Nested attack...\n");
    TERMINAL_VIEW_ADD_TEXT("Known: Block %d, Target: Block %d\n", known_block, target_block);
    
    // First ensure we have a card and it's in reader mode
    if (!chameleon_manager_scan_hf()) {
        printf("Failed to scan HF card first\n");
        TERMINAL_VIEW_ADD_TEXT("Failed to scan HF card first\n");
        return false;
    }
    
    // Initialize nested data structure
    memset(&g_nested_data, 0, sizeof(g_nested_data));
    g_nested_data.timestamp = time(NULL);
    
    // Copy card and attack parameters
    if (g_last_hf_scan.valid && g_last_hf_scan.uid_size >= 4) {
        memcpy(g_nested_data.uid, g_last_hf_scan.uid, 4);
        memcpy(g_nested_data.known_key, known_key, 6);
        g_nested_data.known_block = known_block;
        g_nested_data.known_key_type = known_key_type;
        g_nested_data.target_block = target_block;
        g_nested_data.target_key_type = target_key_type;
        
        printf("Card UID: %02X%02X%02X%02X\n", 
               g_nested_data.uid[0], g_nested_data.uid[1], 
               g_nested_data.uid[2], g_nested_data.uid[3]);
    } else {
        printf("Invalid UID from card scan\n");
        TERMINAL_VIEW_ADD_TEXT("Invalid UID from card scan\n");
        return false;
    }
    
    // Prepare nested attack command
    // Command format: known_block + known_key_type + known_key(6) + target_block + target_key_type
    uint8_t nested_cmd[10];
    nested_cmd[0] = known_block;
    nested_cmd[1] = known_key_type;
    memcpy(&nested_cmd[2], known_key, 6);
    nested_cmd[8] = target_block;
    nested_cmd[9] = target_key_type;
    
    printf("Executing Nested attack (this may take several seconds)...\n");
    TERMINAL_VIEW_ADD_TEXT("Executing Nested attack...\n");
    
    g_response_received = false;
    bool result = send_command(CMD_MF1_NESTED_ACQUIRE, nested_cmd, 10);
    if (!result) {
        printf("Failed to send Nested attack command\n");
        TERMINAL_VIEW_ADD_TEXT("Failed to send Nested attack command\n");
        return false;
    }
    
    // Wait for nested response (this can take longer than normal commands)
    if (xSemaphoreTake(g_response_sem, pdMS_TO_TICKS(20000)) == pdTRUE) {
        if (g_response_received && g_last_response.command == CMD_MF1_NESTED_ACQUIRE) {
            if (g_last_response.status == STATUS_SUCCESS || g_last_response.status == 0x00) {
                printf("Nested attack data collected successfully!\n");
                TERMINAL_VIEW_ADD_TEXT("Nested attack data collected successfully!\n");
                
                // Parse nested response data
                if (g_last_response.data_size >= 16) {
                    // Extract nonces and authentication data
                    // Format typically: nt1(4) + nt2(4) + nr(4) + ar(4)
                    memcpy(&g_nested_data.nt1, &g_last_response.data[0], 4);
                    memcpy(&g_nested_data.nt2, &g_last_response.data[4], 4);
                    memcpy(&g_nested_data.nr, &g_last_response.data[8], 4);
                    memcpy(&g_nested_data.ar, &g_last_response.data[12], 4);
                    
                    // Convert from network byte order if needed
                    g_nested_data.nt1 = __builtin_bswap32(g_nested_data.nt1);
                    g_nested_data.nt2 = __builtin_bswap32(g_nested_data.nt2);
                    g_nested_data.nr = __builtin_bswap32(g_nested_data.nr);
                    g_nested_data.ar = __builtin_bswap32(g_nested_data.ar);
                    
                    g_nested_data.valid = true;
                    
                    printf("Nested Data Collected:\n");
                    printf("  NT1: %08" PRIX32 "\n", g_nested_data.nt1);
                    printf("  NT2: %08" PRIX32 "\n", g_nested_data.nt2);
                    printf("  NR:  %08" PRIX32 "\n", g_nested_data.nr);
                    printf("  AR:  %08" PRIX32 "\n", g_nested_data.ar);
                    
                    TERMINAL_VIEW_ADD_TEXT("Nested data collected:\n");
                    TERMINAL_VIEW_ADD_TEXT("NT1: %08" PRIX32 ", NT2: %08" PRIX32 "\n", g_nested_data.nt1, g_nested_data.nt2);
                    TERMINAL_VIEW_ADD_TEXT("NR: %08" PRIX32 ", AR: %08" PRIX32 "\n", g_nested_data.nr, g_nested_data.ar);
                    
                    printf("\nNote: Use this data with offline tools like 'mfcuk' or 'libnfc-mfcuk' to recover the key.\n");
                    TERMINAL_VIEW_ADD_TEXT("Use data with offline tools to recover the key.\n");
                    
                    return true;
                } else {
                    printf("Insufficient data received from Nested attack (got %d bytes, expected 16+)\n", 
                           g_last_response.data_size);
                    TERMINAL_VIEW_ADD_TEXT("Insufficient data from Nested attack\n");
                }
            } else {
                printf("Nested attack failed with status: 0x%02X\n", g_last_response.status);
                TERMINAL_VIEW_ADD_TEXT("Nested attack failed with status: 0x%02X\n", g_last_response.status);
                
                // Provide some guidance on common failure reasons
                if (g_last_response.status == 0x01) {
                    printf("Known key authentication failed - verify the key is correct\n");
                    TERMINAL_VIEW_ADD_TEXT("Known key authentication failed\n");
                } else if (g_last_response.status == 0x60) {
                    printf("Authentication error - check key and block numbers\n");
                    TERMINAL_VIEW_ADD_TEXT("Authentication error\n");
                }
            }
        }
    } else {
        printf("Nested attack command timed out\n");
        TERMINAL_VIEW_ADD_TEXT("Nested attack command timed out\n");
    }
    
    return false;
}

/**
 * @brief Save Nested attack data to SD card for offline analysis
 * @param filename Custom filename (optional)
 * @return true if data was saved successfully
 */
bool chameleon_manager_save_nested_data(const char* filename) {
    if (!g_nested_data.valid) {
        printf("No Nested attack data to save\n");
        TERMINAL_VIEW_ADD_TEXT("No Nested attack data to save\n");
        return false;
    }
    
    // Create filename if not provided
    char file_path[128];
    if (filename == NULL) {
        struct tm* time_info = localtime(&g_nested_data.timestamp);
        snprintf(file_path, sizeof(file_path), "/mnt/ghostesp/chameleon/nested_%02X%02X%02X%02X_%04d%02d%02d_%02d%02d%02d.txt",
                g_nested_data.uid[0], g_nested_data.uid[1], g_nested_data.uid[2], g_nested_data.uid[3],
                time_info->tm_year + 1900, time_info->tm_mon + 1, time_info->tm_mday,
                time_info->tm_hour, time_info->tm_min, time_info->tm_sec);
    } else {
        snprintf(file_path, sizeof(file_path), "/mnt/ghostesp/chameleon/%s", filename);
    }
    
    // Ensure directory exists
    sd_card_create_directory("/mnt/ghostesp/chameleon");
    
    // Create nested data content
    char content[1024];
    int len = snprintf(content, sizeof(content),
        "MIFARE Classic Nested Attack Data\n"
        "==================================\n"
        "Timestamp: %s"
        "Card UID: %02X%02X%02X%02X\n"
        "Known Block: %d\n"
        "Known Key Type: %s\n"
        "Known Key: %02X%02X%02X%02X%02X%02X\n"
        "Target Block: %d\n"
        "Target Key Type: %s\n\n"
        "Attack Data:\n"
        "NT1: %08" PRIX32 "\n"
        "NT2: %08" PRIX32 "\n"
        "NR:  %08" PRIX32 "\n"
        "AR:  %08" PRIX32 "\n\n"
        "Usage Instructions:\n"
        "==================\n"
        "Use this data with offline MIFARE cracking tools:\n\n"
        "mfcuk method:\n"
        "mfcuk -C -R %d:%08" PRIX32 " -s 200 -S 200\n\n"
        "libnfc method:\n"
        "nfc-mfcuk -k %02X%02X%02X%02X%02X%02X -n %08" PRIX32 "\n\n"
        "Note: Nested attacks use known keys to recover unknown keys.\n"
        "The recovered key can then be used for card cloning or further analysis.\n",
        ctime(&g_nested_data.timestamp),
        g_nested_data.uid[0], g_nested_data.uid[1], g_nested_data.uid[2], g_nested_data.uid[3],
        g_nested_data.known_block,
        (g_nested_data.known_key_type == MF_KEY_A) ? "A" : "B",
        g_nested_data.known_key[0], g_nested_data.known_key[1], g_nested_data.known_key[2],
        g_nested_data.known_key[3], g_nested_data.known_key[4], g_nested_data.known_key[5],
        g_nested_data.target_block,
        (g_nested_data.target_key_type == MF_KEY_A) ? "A" : "B",
        g_nested_data.nt1, g_nested_data.nt2,
        g_nested_data.nr, g_nested_data.ar,
        g_nested_data.known_block, g_nested_data.nt1,
        g_nested_data.known_key[0], g_nested_data.known_key[1], g_nested_data.known_key[2],
        g_nested_data.known_key[3], g_nested_data.known_key[4], g_nested_data.known_key[5],
        g_nested_data.nt1);
    
    // Write to SD card
    esp_err_t result = sd_card_write_file(file_path, content, len);
    if (result == ESP_OK) {
        printf("Nested data saved to: %s\n", file_path);
        TERMINAL_VIEW_ADD_TEXT("Nested data saved to: %s\n", file_path);
        return true;
    } else {
        printf("Failed to save Nested data\n");
        TERMINAL_VIEW_ADD_TEXT("Failed to save Nested data\n");
        return false;
    }
}

/**
 * @brief Perform MIFARE Classic Darkside attack to recover keys
 * @param block Target block number  
 * @param key_type Key type (MF_KEY_A or MF_KEY_B)
 * @return true if attack data was collected successfully
 */
bool chameleon_manager_darkside_attack(uint8_t block, uint8_t key_type) {
    if (!g_is_connected) {
        printf("Not connected to Chameleon Ultra\n");
        TERMINAL_VIEW_ADD_TEXT("Not connected to Chameleon Ultra\n");
        return false;
    }
    
    printf("Starting MIFARE Classic Darkside attack...\n");
    printf("Target: Block %d, Key Type: %s\n", block, (key_type == MF_KEY_A) ? "A" : "B");
    TERMINAL_VIEW_ADD_TEXT("Starting MIFARE Classic Darkside attack...\n");
    TERMINAL_VIEW_ADD_TEXT("Target: Block %d, Key Type: %s\n", block, (key_type == MF_KEY_A) ? "A" : "B");
    
    // First ensure we have a card and it's in reader mode
    if (!chameleon_manager_scan_hf()) {
        printf("Failed to scan HF card first\n");
        TERMINAL_VIEW_ADD_TEXT("Failed to scan HF card first\n");
        return false;
    }
    
    // Initialize darkside data structure
    memset(&g_darkside_data, 0, sizeof(g_darkside_data));
    
    // Copy UID from last scan
    if (g_last_hf_scan.valid && g_last_hf_scan.uid_size >= 4) {
        memcpy(g_darkside_data.uid, g_last_hf_scan.uid, 4);
        g_darkside_data.block = block;
        g_darkside_data.key_type = key_type;
        
        printf("Card UID: %02X%02X%02X%02X\n", 
               g_darkside_data.uid[0], g_darkside_data.uid[1], 
               g_darkside_data.uid[2], g_darkside_data.uid[3]);
        TERMINAL_VIEW_ADD_TEXT("Card UID: %02X%02X%02X%02X\n",
               g_darkside_data.uid[0], g_darkside_data.uid[1], 
               g_darkside_data.uid[2], g_darkside_data.uid[3]);
    } else {
        printf("Invalid UID from card scan\n");
        TERMINAL_VIEW_ADD_TEXT("Invalid UID from card scan\n");
        return false;
    }
    
    // Prepare darkside attack command
    // Command format: block + key_type
    uint8_t darkside_cmd[2];
    darkside_cmd[0] = block;
    darkside_cmd[1] = key_type;
    
    printf("Executing Darkside attack (this may take several seconds)...\n");
    TERMINAL_VIEW_ADD_TEXT("Executing Darkside attack...\n");
    
    g_response_received = false;
    bool result = send_command(CMD_MF1_DARKSIDE_ACQUIRE, darkside_cmd, 2);
    if (!result) {
        printf("Failed to send Darkside attack command\n");
        TERMINAL_VIEW_ADD_TEXT("Failed to send Darkside attack command\n");
        return false;
    }
    
    // Wait for darkside response (this can take longer than normal commands)
    if (xSemaphoreTake(g_response_sem, pdMS_TO_TICKS(15000)) == pdTRUE) {
        if (g_response_received && g_last_response.command == CMD_MF1_DARKSIDE_ACQUIRE) {
            if (g_last_response.status == STATUS_SUCCESS || g_last_response.status == 0x00) {
                printf("Darkside attack data collected successfully!\n");
                TERMINAL_VIEW_ADD_TEXT("Darkside attack data collected successfully!\n");
                
                // Parse darkside response data
                if (g_last_response.data_size >= 16) {
                    // Extract nonces and authentication data
                    // Format typically: nt1(4) + nt2(4) + nr(4) + ar(4)
                    memcpy(&g_darkside_data.nt1, &g_last_response.data[0], 4);
                    memcpy(&g_darkside_data.nt2, &g_last_response.data[4], 4);
                    memcpy(&g_darkside_data.nr, &g_last_response.data[8], 4);
                    memcpy(&g_darkside_data.ar, &g_last_response.data[12], 4);
                    
                    // Convert from network byte order if needed
                    g_darkside_data.nt1 = __builtin_bswap32(g_darkside_data.nt1);
                    g_darkside_data.nt2 = __builtin_bswap32(g_darkside_data.nt2);
                    g_darkside_data.nr = __builtin_bswap32(g_darkside_data.nr);
                    g_darkside_data.ar = __builtin_bswap32(g_darkside_data.ar);
                    
                    g_darkside_data.valid = true;
                    
                    printf("Darkside Data Collected:\n");
                    printf("  NT1: %08" PRIX32 "\n", g_darkside_data.nt1);
                    printf("  NT2: %08" PRIX32 "\n", g_darkside_data.nt2);
                    printf("  NR:  %08" PRIX32 "\n", g_darkside_data.nr);
                    printf("  AR:  %08" PRIX32 "\n", g_darkside_data.ar);
                    
                    TERMINAL_VIEW_ADD_TEXT("Darkside data collected:\n");
                    TERMINAL_VIEW_ADD_TEXT("NT1: %08" PRIX32 ", NT2: %08" PRIX32 "\n", g_darkside_data.nt1, g_darkside_data.nt2);
                    TERMINAL_VIEW_ADD_TEXT("NR: %08" PRIX32 ", AR: %08" PRIX32 "\n", g_darkside_data.nr, g_darkside_data.ar);
                    
                    printf("\nNote: Use this data with offline tools like 'mfcuk' or 'mfoc' to recover the key.\n");
                    TERMINAL_VIEW_ADD_TEXT("Use data with offline tools to recover the key.\n");
                    
                    return true;
                } else {
                    printf("Insufficient data received from Darkside attack (got %d bytes, expected 16+)\n", 
                           g_last_response.data_size);
                    TERMINAL_VIEW_ADD_TEXT("Insufficient data from Darkside attack\n");
                }
            } else {
                printf("Darkside attack failed with status: 0x%02X\n", g_last_response.status);
                TERMINAL_VIEW_ADD_TEXT("Darkside attack failed with status: 0x%02X\n", g_last_response.status);
                
                // Provide some guidance on common failure reasons
                if (g_last_response.status == 0x01) {
                    printf("Card may not be vulnerable to Darkside attack (fixed nonce)\n");
                    TERMINAL_VIEW_ADD_TEXT("Card may not be vulnerable to Darkside attack\n");
                } else if (g_last_response.status == 0x60) {
                    printf("Authentication error - card may be using non-standard implementation\n");
                    TERMINAL_VIEW_ADD_TEXT("Authentication error\n");
                }
            }
        }
    } else {
        printf("Darkside attack command timed out\n");
        TERMINAL_VIEW_ADD_TEXT("Darkside attack command timed out\n");
    }
    
    return false;
}

/**
 * @brief Save Darkside attack data to SD card for offline analysis
 * @param filename Custom filename (optional)
 * @return true if data was saved successfully
 */
bool chameleon_manager_save_darkside_data(const char* filename) {
    if (!g_darkside_data.valid) {
        printf("No Darkside attack data to save\n");
        TERMINAL_VIEW_ADD_TEXT("No Darkside attack data to save\n");
        return false;
    }
    
    // Create filename if not provided
    char file_path[128];
    time_t now = time(NULL);
    if (filename == NULL) {
        struct tm* time_info = localtime(&now);
        snprintf(file_path, sizeof(file_path), "/mnt/ghostesp/chameleon/darkside_%02X%02X%02X%02X_%04d%02d%02d_%02d%02d%02d.txt",
                g_darkside_data.uid[0], g_darkside_data.uid[1], g_darkside_data.uid[2], g_darkside_data.uid[3],
                time_info->tm_year + 1900, time_info->tm_mon + 1, time_info->tm_mday,
                time_info->tm_hour, time_info->tm_min, time_info->tm_sec);
    } else {
        snprintf(file_path, sizeof(file_path), "/mnt/ghostesp/chameleon/%s", filename);
    }
    
    // Ensure directory exists
    sd_card_create_directory("/mnt/ghostesp/chameleon");
    
    // Create darkside data content
    char content[1024];
    int len = snprintf(content, sizeof(content),
        "MIFARE Classic Darkside Attack Data\n"
        "===================================\n"
        "Timestamp: %s"
        "Card UID: %02X%02X%02X%02X\n"
        "Target Block: %d\n"
        "Key Type: %s\n\n"
        "Attack Data:\n"
        "NT1: %08" PRIX32 "\n"
        "NT2: %08" PRIX32 "\n"
        "NR:  %08" PRIX32 "\n"
        "AR:  %08" PRIX32 "\n\n"
        "Usage Instructions:\n"
        "==================\n"
        "Use this data with offline MIFARE cracking tools:\n\n"
        "mfcuk method:\n"
        "mfcuk -C -R 0:%08" PRIX32 " -s 200 -S 200\n\n"
        "mfoc method:\n"
        "mfoc -O key.dmp\n\n"
        "Note: These tools require the nonce data to recover the key.\n"
        "The recovered key can then be used for card cloning or further analysis.\n",
        ctime(&now),
        g_darkside_data.uid[0], g_darkside_data.uid[1], g_darkside_data.uid[2], g_darkside_data.uid[3],
        g_darkside_data.block,
        (g_darkside_data.key_type == MF_KEY_A) ? "A" : "B",
        g_darkside_data.nt1, g_darkside_data.nt2,
        g_darkside_data.nr, g_darkside_data.ar,
        g_darkside_data.nt1);
    
    // Write to SD card
    esp_err_t result = sd_card_write_file(file_path, content, len);
    if (result == ESP_OK) {
        printf("Darkside data saved to: %s\n", file_path);
        TERMINAL_VIEW_ADD_TEXT("Darkside data saved to: %s\n", file_path);
        return true;
    } else {
        printf("Failed to save Darkside data\n");
        TERMINAL_VIEW_ADD_TEXT("Failed to save Darkside data\n");
        return false;
    }
}

bool chameleon_manager_read_hf_card(void) {
    if (!g_is_connected) {
        printf("Not connected to Chameleon Ultra\n");
        TERMINAL_VIEW_ADD_TEXT("Not connected to Chameleon Ultra\n");
        return false;
    }
    
    printf("Starting comprehensive MIFARE Classic card analysis...\n");
    TERMINAL_VIEW_ADD_TEXT("Starting comprehensive MIFARE Classic card analysis...\n");
    
    // First scan to get card info
    if (!chameleon_manager_scan_hf()) {
        printf("Failed to scan HF card first\n");
        TERMINAL_VIEW_ADD_TEXT("Failed to scan HF card first\n");
        return false;
    }
    
    // Initialize data structures
    memset(&g_last_card_dump, 0, sizeof(g_last_card_dump));
    memset(g_sector_auth, 0, sizeof(g_sector_auth));
    g_last_card_dump.valid = true;
    g_last_card_dump.timestamp = time(NULL);
    
    // Copy basic info from scan
    if (g_last_hf_scan.valid) {
        memcpy(g_last_card_dump.uid, g_last_hf_scan.uid, g_last_hf_scan.uid_size);
        g_last_card_dump.uid_size = g_last_hf_scan.uid_size;
        strncpy(g_last_card_dump.tag_type, g_last_hf_scan.tag_type, sizeof(g_last_card_dump.tag_type));
    }
    
    printf("Phase 1: Attempting authentication with default keys...\n");
    TERMINAL_VIEW_ADD_TEXT("Phase 1: Attempting authentication with default keys...\n");
    
    // Try to read all 64 blocks (MIFARE Classic 1K)
    uint16_t total_blocks = 64;
    uint16_t total_sectors = 16;
    uint16_t successful_reads = 0;
    uint16_t sectors_authenticated = 0;
    uint16_t sectors_failed_auth = 0;
    
    // Phase 1: Try default key authentication for each sector
    for (uint8_t sector = 0; sector < total_sectors; sector++) {
        printf("Sector %d: Testing default keys...\n", sector);
        
        uint8_t sector_first_block = sector * 4;
        uint8_t found_key[6];
        
        // Try Key A
        if (authenticate_mifare_block(sector_first_block, MF_KEY_A, found_key)) {
            g_sector_auth[sector].auth_success_a = true;
            memcpy(g_sector_auth[sector].key_a.key, found_key, 6);
            g_sector_auth[sector].key_a.valid = true;
            g_sector_auth[sector].key_a.recovered_by_darkside = false;
            sectors_authenticated++;
            
            printf("  Key A found: %02X%02X%02X%02X%02X%02X\n", 
                   found_key[0], found_key[1], found_key[2], found_key[3], found_key[4], found_key[5]);
        }
        
        // Try Key B
        if (authenticate_mifare_block(sector_first_block, MF_KEY_B, found_key)) {
            g_sector_auth[sector].auth_success_b = true;
            memcpy(g_sector_auth[sector].key_b.key, found_key, 6);
            g_sector_auth[sector].key_b.valid = true;
            g_sector_auth[sector].key_b.recovered_by_darkside = false;
            
            printf("  Key B found: %02X%02X%02X%02X%02X%02X\n", 
                   found_key[0], found_key[1], found_key[2], found_key[3], found_key[4], found_key[5]);
        }
        
        if (!g_sector_auth[sector].auth_success_a && !g_sector_auth[sector].auth_success_b) {
            sectors_failed_auth++;
            printf("  No default keys work\n");
        }
        
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    
    printf("Default key results: %d sectors authenticated, %d failed\n", sectors_authenticated, sectors_failed_auth);
    TERMINAL_VIEW_ADD_TEXT("Default keys: %d sectors authenticated, %d failed\n", sectors_authenticated, sectors_failed_auth);
    
    // Phase 2: Attempt Darkside attacks on failed sectors
    if (sectors_failed_auth > 0) {
        printf("\nPhase 2: Attempting Darkside attacks on protected sectors...\n");
        TERMINAL_VIEW_ADD_TEXT("Phase 2: Attempting Darkside attacks...\n");
        
        for (uint8_t sector = 0; sector < total_sectors; sector++) {
            if (!g_sector_auth[sector].auth_success_a && !g_sector_auth[sector].auth_success_b) {
                printf("Sector %d: Attempting Darkside attack...\n", sector);
                TERMINAL_VIEW_ADD_TEXT("Darkside attack on sector %d...\n", sector);
                
                uint8_t sector_first_block = sector * 4;
                
                // Try Darkside on Key A first
                if (chameleon_manager_darkside_attack(sector_first_block, MF_KEY_A)) {
                    printf("  Darkside attack successful for Key A!\n");
                    printf("  Note: Use offline tools to recover the actual key\n");
                    TERMINAL_VIEW_ADD_TEXT("  Darkside data collected for Key A\n");
                    
                    g_sector_auth[sector].key_a.recovered_by_darkside = true;
                    g_sector_auth[sector].key_a.attack_data_collected = true;
                    
                    // Save darkside data automatically
                    char darkside_filename[64];
                    snprintf(darkside_filename, sizeof(darkside_filename), 
                            "darkside_sector%d_keyA_%02X%02X%02X%02X.txt",
                            sector, g_last_card_dump.uid[0], g_last_card_dump.uid[1], 
                            g_last_card_dump.uid[2], g_last_card_dump.uid[3]);
                    chameleon_manager_save_darkside_data(darkside_filename);
                } else {
                    printf("  Darkside attack failed for Key A\n");
                }
                
                vTaskDelay(pdMS_TO_TICKS(500)); // Delay between attacks
            }
        }
    }
    
    // Phase 2.5: Attempt Nested attacks using known keys
    uint16_t sectors_with_keys = 0;
    for (uint8_t i = 0; i < total_sectors; i++) {
        if (g_sector_auth[i].auth_success_a || g_sector_auth[i].auth_success_b) {
            sectors_with_keys++;
        }
    }
    
    if (sectors_with_keys > 0 && sectors_failed_auth > 0) {
        printf("\nPhase 2.5: Attempting Nested attacks using known keys...\n");
        TERMINAL_VIEW_ADD_TEXT("Phase 2.5: Attempting Nested attacks...\n");
        printf("Found %d sectors with known keys, targeting %d protected sectors\n", 
               sectors_with_keys, sectors_failed_auth);
        
        // For each protected sector, try nested attacks using any known key
        for (uint8_t target_sector = 0; target_sector < total_sectors; target_sector++) {
            if (!g_sector_auth[target_sector].auth_success_a && !g_sector_auth[target_sector].auth_success_b) {
                // Find a sector with a known key to use for nested attack
                for (uint8_t known_sector = 0; known_sector < total_sectors; known_sector++) {
                    if (g_sector_auth[known_sector].auth_success_a) {
                        printf("Sector %d: Nested attack using known key from sector %d...\n", 
                               target_sector, known_sector);
                        TERMINAL_VIEW_ADD_TEXT("Nested attack: sector %d -> sector %d\n", 
                               known_sector, target_sector);
                        
                        uint8_t known_block = known_sector * 4;
                        uint8_t target_block = target_sector * 4;
                        
                        // Try to recover Key A for target sector
                        if (chameleon_manager_nested_attack(known_block, MF_KEY_A, 
                                                          g_sector_auth[known_sector].key_a.key,
                                                          target_block, MF_KEY_A)) {
                            printf("  Nested attack successful for Key A!\n");
                            TERMINAL_VIEW_ADD_TEXT("  Nested data collected for Key A\n");
                            
                            g_sector_auth[target_sector].key_a.recovered_by_nested = true;
                            g_sector_auth[target_sector].key_a.attack_data_collected = true;
                            
                            // Save nested data automatically
                            char nested_filename[64];
                            snprintf(nested_filename, sizeof(nested_filename), 
                                    "nested_sector%d_keyA_%02X%02X%02X%02X.txt",
                                    target_sector, g_last_card_dump.uid[0], g_last_card_dump.uid[1], 
                                    g_last_card_dump.uid[2], g_last_card_dump.uid[3]);
                            chameleon_manager_save_nested_data(nested_filename);
                        } else {
                            printf("  Nested attack failed for Key A\n");
                        }
                        
                        // Stop after first successful nested attack per sector
                        break;
                    }
                }
                
                vTaskDelay(pdMS_TO_TICKS(500)); // Delay between attacks
            }
        }
    }
    
    // Phase 3: Read all accessible blocks using authenticated sectors
    printf("\nPhase 3: Reading card data from authenticated sectors...\n");
    TERMINAL_VIEW_ADD_TEXT("Phase 3: Reading card data...\n");
    
    for (uint16_t block = 0; block < total_blocks; block++) {
        // Skip trailer blocks (every 4th block starting from 3) as they contain keys
        if ((block + 1) % 4 == 0) {
            continue;
        }
        
        uint8_t sector = block / 4;
        
        // Only read if we have authentication for this sector
        if (g_sector_auth[sector].auth_success_a || g_sector_auth[sector].auth_success_b) {
            // Re-authenticate before reading (choose Key A if available)
            bool reauth_success = false;
            if (g_sector_auth[sector].auth_success_a) {
                if (authenticate_mifare_block(sector * 4, MF_KEY_A, NULL)) {
                    reauth_success = true;
                }
            } else if (g_sector_auth[sector].auth_success_b) {
                if (authenticate_mifare_block(sector * 4, MF_KEY_B, NULL)) {
                    reauth_success = true;
                }
            }
            
            if (reauth_success) {
                // Now try to read the block
                uint8_t block_data[1] = {(uint8_t)block};
                
                g_response_received = false;
                bool result = send_command(CMD_MF1_READ_ONE_BLOCK, block_data, 1);
                if (result && xSemaphoreTake(g_response_sem, pdMS_TO_TICKS(2000)) == pdTRUE) {
                    if (g_response_received && g_last_response.command == CMD_MF1_READ_ONE_BLOCK) {
                        if ((g_last_response.status == STATUS_SUCCESS || g_last_response.status == 0x00) && 
                            g_last_response.data_size == 16) {
                            // Copy block data
                            memcpy(g_last_card_dump.blocks[block], g_last_response.data, 16);
                            g_last_card_dump.block_valid[block] = true;
                            successful_reads++;
                            
                            printf("Block %d: ", block);
                            for (int i = 0; i < 16; i++) {
                                printf("%02X ", g_last_response.data[i]);
                            }
                            printf("\n");
                        }
                    }
                }
                
                vTaskDelay(pdMS_TO_TICKS(50));
            }
        }
    }
    
    g_last_card_dump.total_blocks_read = successful_reads;
    g_last_card_dump.card_size_blocks = total_blocks;
    
    // Final summary
    printf("\n=== CARD ANALYSIS COMPLETE ===\n");
    printf("Blocks read: %d/%d\n", successful_reads, total_blocks - 16); // -16 for trailer blocks
    printf("Sectors with default keys: %d/%d\n", sectors_authenticated, total_sectors);
    printf("Sectors requiring Darkside: %d/%d\n", sectors_failed_auth, total_sectors);
    
    TERMINAL_VIEW_ADD_TEXT("=== ANALYSIS COMPLETE ===\n");
    TERMINAL_VIEW_ADD_TEXT("Blocks read: %d/%d\n", successful_reads, total_blocks - 16);
    TERMINAL_VIEW_ADD_TEXT("Default keys: %d/%d sectors\n", sectors_authenticated, total_sectors);
    TERMINAL_VIEW_ADD_TEXT("Darkside needed: %d/%d sectors\n", sectors_failed_auth, total_sectors);
    
    return true;
}

bool chameleon_manager_read_lf_card(void) {
    printf("LF card full dump not yet implemented\n");
    TERMINAL_VIEW_ADD_TEXT("LF card full dump not yet implemented\n");
    return false;
}

bool chameleon_manager_save_card_dump(const char* filename) {
    if (!g_last_card_dump.valid) {
        printf("No card dump data to save\n");
        TERMINAL_VIEW_ADD_TEXT("No card dump data to save\n");
        return false;
    }
    
    // Create filename if not provided
    char file_path[128];
    if (filename == NULL) {
        struct tm* time_info = localtime(&g_last_card_dump.timestamp);
        snprintf(file_path, sizeof(file_path), "/mnt/ghostesp/chameleon/card_dump_%04d%02d%02d_%02d%02d%02d.bin",
                time_info->tm_year + 1900, time_info->tm_mon + 1, time_info->tm_mday,
                time_info->tm_hour, time_info->tm_min, time_info->tm_sec);
    } else {
        snprintf(file_path, sizeof(file_path), "/mnt/ghostesp/chameleon/%s", filename);
    }
    
    // Ensure directory exists
    sd_card_create_directory("/mnt/ghostesp/chameleon");
    
    // Create comprehensive dump content
    char content[8192];
    int len = snprintf(content, sizeof(content),
        "Chameleon Ultra Card Dump\n"
        "=========================\n"
        "Timestamp: %s"
        "Tag Type: %s\n"
        "UID Size: %d bytes\n"
        "UID: ",
        ctime(&g_last_card_dump.timestamp),
        g_last_card_dump.tag_type,
        g_last_card_dump.uid_size);
    
    // Add UID
    for (int i = 0; i < g_last_card_dump.uid_size && i < 20; i++) {
        len += snprintf(content + len, sizeof(content) - len, "%02X ", g_last_card_dump.uid[i]);
    }
    
    len += snprintf(content + len, sizeof(content) - len, 
        "\nTotal Blocks Attempted: %d\n"
        "Blocks Successfully Read: %d\n"
        "Analysis Method: Comprehensive (Default Keys + Darkside Attacks)\n\n"
        "Sector Authentication Summary:\n"
        "==============================\n",
        g_last_card_dump.card_size_blocks,
        g_last_card_dump.total_blocks_read);
    
    // Add sector authentication details
    for (int sector = 0; sector < MAX_SECTORS; sector++) {
        len += snprintf(content + len, sizeof(content) - len, "Sector %02d: ", sector);
        
        if (g_sector_auth[sector].auth_success_a) {
            const char* method = "";
            if (g_sector_auth[sector].key_a.recovered_by_darkside) method = "(DS)";
            else if (g_sector_auth[sector].key_a.recovered_by_nested) method = "(N)";
            
            len += snprintf(content + len, sizeof(content) - len, "Key A=%02X%02X%02X%02X%02X%02X%s ",
                g_sector_auth[sector].key_a.key[0], g_sector_auth[sector].key_a.key[1],
                g_sector_auth[sector].key_a.key[2], g_sector_auth[sector].key_a.key[3],
                g_sector_auth[sector].key_a.key[4], g_sector_auth[sector].key_a.key[5], method);
        } else if (g_sector_auth[sector].key_a.recovered_by_darkside) {
            len += snprintf(content + len, sizeof(content) - len, "Key A=Darkside_Data ");
        } else if (g_sector_auth[sector].key_a.recovered_by_nested) {
            len += snprintf(content + len, sizeof(content) - len, "Key A=Nested_Data ");
        } else {
            len += snprintf(content + len, sizeof(content) - len, "Key A=Unknown ");
        }
        
        if (g_sector_auth[sector].auth_success_b) {
            const char* method = "";
            if (g_sector_auth[sector].key_b.recovered_by_darkside) method = "(DS)";
            else if (g_sector_auth[sector].key_b.recovered_by_nested) method = "(N)";
            
            len += snprintf(content + len, sizeof(content) - len, "Key B=%02X%02X%02X%02X%02X%02X%s",
                g_sector_auth[sector].key_b.key[0], g_sector_auth[sector].key_b.key[1],
                g_sector_auth[sector].key_b.key[2], g_sector_auth[sector].key_b.key[3],
                g_sector_auth[sector].key_b.key[4], g_sector_auth[sector].key_b.key[5], method);
        } else if (g_sector_auth[sector].key_b.recovered_by_darkside) {
            len += snprintf(content + len, sizeof(content) - len, "Key B=Darkside_Data");
        } else if (g_sector_auth[sector].key_b.recovered_by_nested) {
            len += snprintf(content + len, sizeof(content) - len, "Key B=Nested_Data");
        } else {
            len += snprintf(content + len, sizeof(content) - len, "Key B=Unknown");
        }
        
        len += snprintf(content + len, sizeof(content) - len, "\n");
        
        // Prevent buffer overflow
        if (len >= sizeof(content) - 200) {
            break;
        }
    }
    
    len += snprintf(content + len, sizeof(content) - len, 
        "\nNote: (DS) indicates Darkside attack data, (N) indicates Nested attack data.\n"
        "Use offline tools to recover actual keys from attack data.\n\n"
        "Block Data:\n"
        "===========\n");
    
    // Add block data
    for (uint16_t block = 0; block < g_last_card_dump.card_size_blocks && block < MAX_CARD_BLOCKS; block++) {
        if (g_last_card_dump.block_valid[block]) {
            len += snprintf(content + len, sizeof(content) - len, "Block %03d: ", block);
            for (int i = 0; i < BLOCK_SIZE; i++) {
                len += snprintf(content + len, sizeof(content) - len, "%02X ", g_last_card_dump.blocks[block][i]);
            }
            len += snprintf(content + len, sizeof(content) - len, "\n");
        } else {
            len += snprintf(content + len, sizeof(content) - len, "Block %03d: [UNREAD]\n", block);
        }
        
        // Prevent buffer overflow
        if (len >= sizeof(content) - 100) {
            len += snprintf(content + len, sizeof(content) - len, "\n[TRUNCATED - Buffer full]\n");
            break;
        }
    }
    
    // Write to SD card
    esp_err_t result = sd_card_write_file(file_path, content, len);
    if (result == ESP_OK) {
        printf("Card dump saved to: %s\n", file_path);
        TERMINAL_VIEW_ADD_TEXT("Card dump saved to: %s\n", file_path);
        return true;
    } else {
        printf("Failed to save card dump\n");
        TERMINAL_VIEW_ADD_TEXT("Failed to save card dump\n");
        return false;
    }
}

bool chameleon_manager_save_last_hf_scan(const char* filename) {
    if (!g_last_hf_scan.valid) {
        printf("No HF scan data to save\n");
        TERMINAL_VIEW_ADD_TEXT("No HF scan data to save\n");
        return false;
    }
    
    // Create filename if not provided
    char file_path[128];
    if (filename == NULL) {
        struct tm* time_info = localtime(&g_last_hf_scan.timestamp);
        snprintf(file_path, sizeof(file_path), "/mnt/ghostesp/chameleon/hf_scan_%04d%02d%02d_%02d%02d%02d.txt",
                time_info->tm_year + 1900, time_info->tm_mon + 1, time_info->tm_mday,
                time_info->tm_hour, time_info->tm_min, time_info->tm_sec);
    } else {
        snprintf(file_path, sizeof(file_path), "/mnt/ghostesp/chameleon/%s", filename);
    }
    
    // Ensure directory exists
    sd_card_create_directory("/mnt/ghostesp/chameleon");
    
    // Create file content
    char content[512];
    int len = snprintf(content, sizeof(content),
        "Chameleon Ultra HF Scan Data\n"
        "============================\n"
        "Timestamp: %s"
        "Tag Type: %s\n"
        "UID Size: %d bytes\n"
        "UID: ",
        ctime(&g_last_hf_scan.timestamp),
        g_last_hf_scan.tag_type,
        g_last_hf_scan.uid_size);
    
    // Add UID bytes
    for (int i = 0; i < g_last_hf_scan.uid_size && i < 20; i++) {
        len += snprintf(content + len, sizeof(content) - len, "%02X ", g_last_hf_scan.uid[i]);
    }
    len += snprintf(content + len, sizeof(content) - len, "\n\nRaw Data (hex):\n");
    for (int i = 0; i < g_last_hf_scan.uid_size && i < 20; i++) {
        len += snprintf(content + len, sizeof(content) - len, "%02X", g_last_hf_scan.uid[i]);
    }
    len += snprintf(content + len, sizeof(content) - len, "\n");
    
    // Write to SD card
    esp_err_t result = sd_card_write_file(file_path, content, len);
    if (result == ESP_OK) {
        printf("HF scan data saved to: %s\n", file_path);
        TERMINAL_VIEW_ADD_TEXT("HF scan data saved to: %s\n", file_path);
        return true;
    } else {
        printf("Failed to save HF scan data\n");
        TERMINAL_VIEW_ADD_TEXT("Failed to save HF scan data\n");
        return false;
    }
}

bool chameleon_manager_save_last_lf_scan(const char* filename) {
    if (!g_last_lf_scan.valid) {
        printf("No LF scan data to save\n");
        TERMINAL_VIEW_ADD_TEXT("No LF scan data to save\n");
        return false;
    }
    
    // Create filename if not provided
    char file_path[128];
    if (filename == NULL) {
        struct tm* time_info = localtime(&g_last_lf_scan.timestamp);
        snprintf(file_path, sizeof(file_path), "/mnt/ghostesp/chameleon/lf_scan_%04d%02d%02d_%02d%02d%02d.txt",
                time_info->tm_year + 1900, time_info->tm_mon + 1, time_info->tm_mday,
                time_info->tm_hour, time_info->tm_min, time_info->tm_sec);
    } else {
        snprintf(file_path, sizeof(file_path), "/mnt/ghostesp/chameleon/%s", filename);
    }
    
    // Ensure directory exists
    sd_card_create_directory("/mnt/ghostesp/chameleon");
    
    // Create file content
    char content[512];
    int len = snprintf(content, sizeof(content),
        "Chameleon Ultra LF Scan Data\n"
        "============================\n"
        "Timestamp: %s"
        "Tag Type: %s\n"
        "Data Size: %d bytes\n"
        "Data: ",
        ctime(&g_last_lf_scan.timestamp),
        g_last_lf_scan.tag_type,
        g_last_lf_scan.uid_size);
    
    // Add data bytes
    for (int i = 0; i < g_last_lf_scan.uid_size && i < 20; i++) {
        len += snprintf(content + len, sizeof(content) - len, "%02X ", g_last_lf_scan.uid[i]);
    }
    len += snprintf(content + len, sizeof(content) - len, "\n\nRaw Data (hex):\n");
    for (int i = 0; i < g_last_lf_scan.uid_size && i < 20; i++) {
        len += snprintf(content + len, sizeof(content) - len, "%02X", g_last_lf_scan.uid[i]);
    }
    len += snprintf(content + len, sizeof(content) - len, "\n");
    
    // Write to SD card
    esp_err_t result = sd_card_write_file(file_path, content, len);
    if (result == ESP_OK) {
        printf("LF scan data saved to: %s\n", file_path);
        TERMINAL_VIEW_ADD_TEXT("LF scan data saved to: %s\n", file_path);
        return true;
    } else {
        printf("Failed to save LF scan data\n");
        TERMINAL_VIEW_ADD_TEXT("Failed to save LF scan data\n");
        return false;
    }
}

/**
 * @brief Comprehensive NTAG card detection and type identification
 * @return true if card appears to be an NTAG, false otherwise
 */
bool chameleon_manager_detect_ntag(void) {
    if (!g_is_connected) {
        printf("Not connected to Chameleon Ultra\n");
        TERMINAL_VIEW_ADD_TEXT("Not connected to Chameleon Ultra\n");
        return false;
    }
    
    ESP_LOGI("chameleon_manager", "Starting comprehensive NTAG detection...");
    printf("Detecting NTAG card...\n");
    TERMINAL_VIEW_ADD_TEXT("Detecting NTAG card...\n");
    
    // Clear previous NTAG dump data
    memset(&g_last_ntag_dump, 0, sizeof(g_last_ntag_dump));
    
    // First ensure we have a card and it's in reader mode
    if (!chameleon_manager_scan_hf()) {
        printf("No HF card detected\n");
        TERMINAL_VIEW_ADD_TEXT("No HF card detected\n");
        return false;
    }
    
    if (!g_last_hf_scan.valid) {
        printf("Invalid HF scan data\n");
        return false;
    }
    
    // Initialize NTAG dump structure
    g_last_ntag_dump.valid = false;
    g_last_ntag_dump.uid_size = g_last_hf_scan.uid_size;
    memcpy(g_last_ntag_dump.uid, g_last_hf_scan.uid, g_last_hf_scan.uid_size);
    g_last_ntag_dump.timestamp = time(NULL);
    strcpy(g_last_ntag_dump.card_type, "Unknown NTAG");
    
    // Analyze UID pattern for NTAG characteristics
    printf("Card Analysis:\n");
    printf("   UID (%d bytes): ", g_last_hf_scan.uid_size);
    for (int i = 0; i < g_last_hf_scan.uid_size; i++) {
        printf("%02X", g_last_hf_scan.uid[i]);
        if (i < g_last_hf_scan.uid_size - 1) printf(" ");
    }
    printf("\n");
    
    // Check for NTAG characteristics
    
    // NTAG cards typically have 7-byte UIDs starting with 0x04 (NXP manufacturer)
    if (g_last_hf_scan.uid_size == 7 && g_last_hf_scan.uid[0] == 0x04) {
        strcpy(g_last_ntag_dump.card_type, "NTAG215");  // Default assumption
        printf("   * 7-byte UID starting with 0x04 (NXP) - NTAG pattern detected\n");
    } else if (g_last_hf_scan.uid_size == 4 && g_last_hf_scan.uid[0] == 0x04) {
        strcpy(g_last_ntag_dump.card_type, "NTAG213");  // Smaller cards more likely
        printf("   * 4-byte UID starting with 0x04 (NXP) - possible NTAG213\n");
    } else {
        printf("   ERROR: UID pattern doesn't match typical NTAG cards\n");
        TERMINAL_VIEW_ADD_TEXT("ERROR: Card doesn't appear to be an NTAG\n");
        return false;
    }
    
    // Attempt GET_VERSION command for precise identification
    printf("ANALYZING: Attempting GET_VERSION command for precise identification...\n");
    uint8_t get_version_cmd[1] = {NTAG_GET_VERSION_CMD};
    
    g_response_received = false;
    g_last_ntag_dump.version_valid = false;
    
    if (send_command(CMD_HF14A_RAW, get_version_cmd, 1)) {
        if (xSemaphoreTake(g_response_sem, pdMS_TO_TICKS(3000)) == pdTRUE) {
            if (g_response_received && g_last_response.command == CMD_HF14A_RAW) {
                if (g_last_response.status == STATUS_SUCCESS || g_last_response.status == 0x00) {
                    if (g_last_response.data_size >= 8) {
                        printf("   SUCCESS: GET_VERSION successful! Parsing response...\n");
                        g_last_ntag_dump.version_valid = true;
                        memcpy(g_last_ntag_dump.version_data, g_last_response.data, 8);
                        
                        // Parse version info for exact NTAG type
                        uint8_t vendor = g_last_response.data[1];
                        uint8_t type = g_last_response.data[2];
                        uint8_t subtype = g_last_response.data[3];
                        uint8_t version_major = g_last_response.data[4];
                        uint8_t version_minor = g_last_response.data[5];
                        uint8_t storage_size = g_last_response.data[6];
                        uint8_t protocol = g_last_response.data[7];
                        
                        printf("   DETAILS: Version Details:\n");
                        printf("      Vendor: 0x%02X, Type: 0x%02X, Subtype: 0x%02X\n", vendor, type, subtype);
                        printf("      Version: %d.%d, Storage: 0x%02X, Protocol: 0x%02X\n", 
                               version_major, version_minor, storage_size, protocol);
                        
                        // Determine exact NTAG type based on storage size
                        switch (storage_size) {
                            case 0x0F:
                                strcpy(g_last_ntag_dump.card_type, "NTAG213");
                                g_last_ntag_dump.total_pages = NTAG213_TOTAL_PAGES;
                                g_last_ntag_dump.user_memory_end = 39;
                                printf("   IDENTIFIED: Identified: NTAG213 (180 bytes, 45 pages)\n");
                                break;
                            case 0x11:
                                strcpy(g_last_ntag_dump.card_type, "NTAG215");
                                g_last_ntag_dump.total_pages = NTAG215_TOTAL_PAGES;
                                g_last_ntag_dump.user_memory_end = 129;
                                printf("   IDENTIFIED: Identified: NTAG215 (540 bytes, 135 pages)\n");
                                break;
                            case 0x13:
                                strcpy(g_last_ntag_dump.card_type, "NTAG216");
                                g_last_ntag_dump.total_pages = NTAG216_TOTAL_PAGES;
                                g_last_ntag_dump.user_memory_end = 225;
                                printf("   IDENTIFIED: Identified: NTAG216 (924 bytes, 231 pages)\n");
                                break;
                            default:
                                strcpy(g_last_ntag_dump.card_type, "NTAG215");  // Safe default
                                g_last_ntag_dump.total_pages = NTAG215_TOTAL_PAGES;
                                g_last_ntag_dump.user_memory_end = 129;
                                printf("   WARNING:  Unknown storage size 0x%02X - assuming NTAG215\n", storage_size);
                                break;
                        }
                        
                        g_last_ntag_dump.password_protected = false;  // GET_VERSION worked, so not protected
                        
                    } else {
                        printf("   WARNING:  GET_VERSION response too short (%d bytes)\n", g_last_response.data_size);
                    }
                } else {
                    printf("   WARNING:  GET_VERSION failed (status: 0x%02X)\n", g_last_response.status);
                    if (g_last_response.status == 0x60) {
                        printf("   PROTECTED: Authentication required - card is password protected\n");
                        g_last_ntag_dump.password_protected = true;
                    }
                }
            }
        } else {
            printf("   TIMEOUT: GET_VERSION command timeout\n");
        }
    } else {
        printf("   ERROR: Failed to send GET_VERSION command\n");
    }
    
    // Set default values if GET_VERSION failed
    if (!g_last_ntag_dump.version_valid) {
        // Assume NTAG215 if we can't determine exactly
        if (strcmp(g_last_ntag_dump.card_type, "Unknown NTAG") == 0) {
            strcpy(g_last_ntag_dump.card_type, "NTAG215");
        }
        g_last_ntag_dump.total_pages = NTAG215_TOTAL_PAGES;
        g_last_ntag_dump.user_memory_end = 129;
        g_last_ntag_dump.password_protected = true;  // Assume protected if GET_VERSION failed
    }
    
    // Set common NTAG memory map values
    g_last_ntag_dump.user_memory_start = NTAG_USER_START_PAGE;
    g_last_ntag_dump.lock_bytes_page = 2;  // Standard NTAG lock bytes location
    
    printf("\nSUCCESS: NTAG Detection Complete!\n");
    printf("   INFO: Card Type: %s\n", g_last_ntag_dump.card_type);
    printf("   SIZE: Total Pages: %d\n", g_last_ntag_dump.total_pages);
    printf("   PROTECTED: Password Protected: %s\n", g_last_ntag_dump.password_protected ? "Yes" : "No");
    
    TERMINAL_VIEW_ADD_TEXT("SUCCESS: %s detected", g_last_ntag_dump.card_type);
    if (g_last_ntag_dump.password_protected) {
        TERMINAL_VIEW_ADD_TEXT(" (protected)");
    }
    TERMINAL_VIEW_ADD_TEXT("\n");
    
    g_last_ntag_dump.valid = true;
    return true;
}

/**
 * @brief Attempt NTAG password authentication with common passwords
 * @param password 4-byte password to try
 * @return true if authentication succeeded
 */
bool chameleon_manager_ntag_authenticate(uint32_t password) {
    printf("   KEY: Trying password: %08" PRIX32 "\n", password);
    
    // Prepare PWD_AUTH command: [1B] [PWD0] [PWD1] [PWD2] [PWD3]
    uint8_t auth_cmd[5] = {
        NTAG_PWD_AUTH_CMD,
        (password >> 24) & 0xFF,
        (password >> 16) & 0xFF,
        (password >> 8) & 0xFF,
        password & 0xFF
    };
    
    g_response_received = false;
    if (send_command(CMD_HF14A_RAW, auth_cmd, 5)) {
        if (xSemaphoreTake(g_response_sem, pdMS_TO_TICKS(2000)) == pdTRUE) {
            if (g_response_received && g_last_response.command == CMD_HF14A_RAW) {
                if (g_last_response.status == STATUS_SUCCESS || g_last_response.status == 0x00) {
                    if (g_last_response.data_size >= 2) {
                        printf("   SUCCESS: Password authentication successful!\n");
                        printf("   INFO: PACK response: %02X %02X\n", 
                               g_last_response.data[0], g_last_response.data[1]);
                        return true;
                    }
                } else {
                    ESP_LOGI("chameleon_manager", "Password auth failed: status 0x%02X", g_last_response.status);
                }
            }
        }
    }
    return false;
}

/**
 * @brief Try to read unprotected pages (header pages 0-3 are usually readable)
 * @return number of pages successfully read
 */
static uint16_t read_unprotected_ntag_pages(void) {
    uint16_t pages_read = 0;
    
    printf("   UNPROTECTED: Attempting to read unprotected header pages...\n");
    
    // Try to read the first 4 pages (header) which are usually readable even on protected cards
    uint8_t read_cmd[2] = {NTAG_READ_CMD, 0x00};  // Start from page 0
    
    g_response_received = false;
    if (send_command(CMD_HF14A_RAW, read_cmd, 2)) {
        if (xSemaphoreTake(g_response_sem, pdMS_TO_TICKS(3000)) == pdTRUE) {
            if (g_response_received && g_last_response.command == CMD_HF14A_RAW) {
                if ((g_last_response.status == STATUS_SUCCESS || g_last_response.status == 0x00) && 
                    g_last_response.data_size >= 16) {
                    
                    printf("   SUCCESS: Header pages readable - extracting UID and basic info\n");
                    
                    // Store the 4 header pages
                    for (int i = 0; i < 4; i++) {
                        if (i < NTAG216_TOTAL_PAGES) {
                            memcpy(g_last_ntag_dump.pages[i], &g_last_response.data[i * 4], 4);
                            g_last_ntag_dump.page_valid[i] = true;
                            pages_read++;
                        }
                    }
                    
                    // Extract UID from header
                    if (g_last_ntag_dump.page_valid[0] && g_last_ntag_dump.page_valid[1]) {
                        printf("   INFO: UID from header: ");
                        for (int i = 0; i < 3; i++) {
                            printf("%02X ", g_last_ntag_dump.pages[0][i]);
                        }
                        for (int i = 0; i < 4; i++) {
                            printf("%02X ", g_last_ntag_dump.pages[1][i]);
                        }
                        printf("\n");
                        
                        // Check capability container in page 3
                        if (g_last_ntag_dump.page_valid[3]) {
                            uint8_t cc_magic = g_last_ntag_dump.pages[3][0];
                            uint8_t cc_version = g_last_ntag_dump.pages[3][1];
                            uint8_t cc_size = g_last_ntag_dump.pages[3][2];
                            uint8_t cc_read_write = g_last_ntag_dump.pages[3][3];
                            
                            printf("   PAGES: Capability Container: Magic=0x%02X, Ver=%d.%d, Size=0x%02X, RW=0x%02X\n",
                                   cc_magic, (cc_version >> 4) & 0x0F, cc_version & 0x0F, cc_size, cc_read_write);
                        }
                    }
                }
            }
        }
    }
    
    return pages_read;
}

/**
 * @brief Comprehensive NTAG card dumping with password recovery attempts
 * @return true if any data was readable
 */
bool chameleon_manager_read_ntag_card(void) {
    if (!g_is_connected) {
        printf("Not connected to Chameleon Ultra\n");
        TERMINAL_VIEW_ADD_TEXT("Not connected to Chameleon Ultra\n");
        return false;
    }
    
    ESP_LOGI("chameleon_manager", "Starting comprehensive NTAG card dump...");
    printf("STARTING: Starting NTAG card dump...\n");
    TERMINAL_VIEW_ADD_TEXT("STARTING: Starting NTAG card dump...\n");
    
    // First detect the NTAG type
    if (!chameleon_manager_detect_ntag()) {
        printf("ERROR: Card does not appear to be an NTAG\n");
        TERMINAL_VIEW_ADD_TEXT("ERROR: Not an NTAG card\n");
        return false;
    }
    
    // Initialize page tracking
    g_last_ntag_dump.readable_pages = 0;
    g_last_ntag_dump.protected_pages = 0;
    g_last_ntag_dump.password_found = false;
    
    // Try password authentication if the card appears protected
    if (g_last_ntag_dump.password_protected) {
        printf("\nPROTECTED: Card is password protected - attempting authentication...\n");
        TERMINAL_VIEW_ADD_TEXT("PROTECTED: Attempting password authentication...\n");
        
        size_t num_passwords = sizeof(ntag_default_passwords) / sizeof(ntag_default_passwords[0]);
        for (size_t i = 0; i < num_passwords; i++) {
            if (chameleon_manager_ntag_authenticate(ntag_default_passwords[i])) {
                printf("FOUND: Found working password: %08" PRIX32 "\n", ntag_default_passwords[i]);
                TERMINAL_VIEW_ADD_TEXT("FOUND: Password found: %08" PRIX32 "\n", ntag_default_passwords[i]);
                g_last_ntag_dump.password = ntag_default_passwords[i];
                g_last_ntag_dump.password_found = true;
                g_last_ntag_dump.password_protected = false;  // We can access it now
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(100));  // Small delay between attempts
        }
        
        if (!g_last_ntag_dump.password_found) {
            printf("WARNING:  No working password found in default list\n");
            TERMINAL_VIEW_ADD_TEXT("WARNING:  Default passwords failed\n");
            
            // Try to read at least the unprotected header pages
            printf("\nREADING: Attempting to read unprotected pages...\n");
            uint16_t header_pages = read_unprotected_ntag_pages();
            if (header_pages > 0) {
                printf("   SUCCESS: Successfully read %d header pages\n", header_pages);
                g_last_ntag_dump.readable_pages = header_pages;
                
                // For protected cards, we can still save basic info
                printf("   NOTE: Card is protected but basic information extracted\n");
                TERMINAL_VIEW_ADD_TEXT("SUCCESS: %d header pages read\n", header_pages);
                return true; // Consider this a partial success
            } else {
                printf("   ERROR: Cannot read any pages - card is fully protected\n");
                TERMINAL_VIEW_ADD_TEXT("ERROR: Fully protected card\n");
                return false;
            }
        }
    }
    
    // Check connection before proceeding with dump
    if (!g_is_connected) {
        printf("ERROR: Connection lost during authentication - attempting reconnect...\n");
        TERMINAL_VIEW_ADD_TEXT("ERROR: Connection lost - reconnecting...\n");
        
        // Try to reconnect
        if (!chameleon_manager_connect(10)) {
            printf("ERROR: Failed to reconnect to Chameleon Ultra\n");
            TERMINAL_VIEW_ADD_TEXT("ERROR: Reconnection failed\n");
            return false;
        }
        
        printf("SUCCESS: Reconnected successfully\n");
        TERMINAL_VIEW_ADD_TEXT("SUCCESS: Reconnected\n");
        
        // Re-scan the card after reconnection
        if (!chameleon_manager_scan_hf()) {
            printf("ERROR: Card no longer detected after reconnection\n");
            TERMINAL_VIEW_ADD_TEXT("ERROR: Card lost\n");
            return false;
        }
    }
    
    // Now attempt to read all pages
    printf("\nREADING: Reading NTAG pages...\n");
    printf("   Card Type: %s (%d total pages)\n", g_last_ntag_dump.card_type, g_last_ntag_dump.total_pages);
    TERMINAL_VIEW_ADD_TEXT("READING: Reading pages...\n");
    
    // Read pages in 4-page blocks for better efficiency and connection stability
    uint16_t read_failures = 0;
    uint16_t max_failures = 10;  // Allow some failures before giving up
    
    for (uint16_t page = 0; page < g_last_ntag_dump.total_pages; page += 4) {
        if (page % 20 == 0) {
            printf("   PAGES: Reading pages %d-%d/%d...\n", page, 
                   (page + 3 < g_last_ntag_dump.total_pages) ? page + 3 : g_last_ntag_dump.total_pages - 1,
                   g_last_ntag_dump.total_pages - 1);
        }
        
        // Check connection health periodically
        if (page > 0 && page % 40 == 0) {
            if (!g_is_connected) {
                printf("   WARNING:  Connection lost at page %d - attempting reconnect...\n", page);
                if (!chameleon_manager_connect(5)) {
                    printf("   ERROR: Reconnection failed - stopping dump\n");
                    break;
                }
                if (!chameleon_manager_scan_hf()) {
                    printf("   ERROR: Card lost after reconnection - stopping dump\n");
                    break;
                }
                printf("   SUCCESS: Reconnected and card re-detected\n");
            }
        }
        
        uint8_t read_cmd[2] = {NTAG_READ_CMD, (uint8_t)page};
        
        g_response_received = false;
        bool result = send_command(CMD_HF14A_RAW, read_cmd, 2);
        
        if (!result) {
            ESP_LOGI("chameleon_manager", "Failed to send read command for page %d", page);
            read_failures++;
            if (read_failures > max_failures) {
                printf("   ERROR: Too many read failures (%d) - stopping dump\n", read_failures);
                break;
            }
            continue;
        }
        
        if (xSemaphoreTake(g_response_sem, pdMS_TO_TICKS(3000)) == pdTRUE) {
            if (g_response_received && g_last_response.command == CMD_HF14A_RAW) {
                if (g_last_response.status == STATUS_SUCCESS || g_last_response.status == 0x00) {
                    if (g_last_response.data_size >= 16) {
                        // NTAG READ returns 4 pages (16 bytes) starting from the requested page
                        for (int i = 0; i < 4 && (page + i) < g_last_ntag_dump.total_pages; i++) {
                            uint16_t current_page = page + i;
                            if (current_page < NTAG216_TOTAL_PAGES) {
                                memcpy(g_last_ntag_dump.pages[current_page], 
                                       &g_last_response.data[i * 4], 4);
                                g_last_ntag_dump.page_valid[current_page] = true;
                                g_last_ntag_dump.readable_pages++;
                            }
                        }
                        
                        // Reset failure counter on successful read
                        read_failures = 0;
                        
                    } else {
                        ESP_LOGI("chameleon_manager", "Page %d: insufficient data (%d bytes)", 
                                page, g_last_response.data_size);
                        for (int i = 0; i < 4 && (page + i) < g_last_ntag_dump.total_pages; i++) {
                            g_last_ntag_dump.protected_pages++;
                        }
                        read_failures++;
                    }
                } else {
                    if (g_last_response.status == 0x60) {
                        ESP_LOGI("chameleon_manager", "Page %d: authentication required", page);
                    } else {
                        ESP_LOGI("chameleon_manager", "Page %d: read failed (status 0x%02X)", 
                                page, g_last_response.status);
                    }
                    for (int i = 0; i < 4 && (page + i) < g_last_ntag_dump.total_pages; i++) {
                        g_last_ntag_dump.protected_pages++;
                    }
                    read_failures++;
                }
            } else {
                ESP_LOGI("chameleon_manager", "Page %d: no response received", page);
                for (int i = 0; i < 4 && (page + i) < g_last_ntag_dump.total_pages; i++) {
                    g_last_ntag_dump.protected_pages++;
                }
                read_failures++;
            }
        } else {
            ESP_LOGI("chameleon_manager", "Page %d: read timeout", page);
            for (int i = 0; i < 4 && (page + i) < g_last_ntag_dump.total_pages; i++) {
                g_last_ntag_dump.protected_pages++;
            }
            read_failures++;
        }
        
        // Check if we've hit failure limit
        if (read_failures > max_failures) {
            printf("   ERROR: Too many consecutive failures (%d) - stopping dump\n", read_failures);
            break;
        }
        
        // Small delay to avoid overwhelming the connection
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    
    // Analyze NDEF data if present
    g_last_ntag_dump.ndef_present = false;
    if (g_last_ntag_dump.page_valid[4]) {  // NDEF typically starts at page 4
        uint8_t ndef_header = g_last_ntag_dump.pages[4][0];
        if (ndef_header == 0x03) {  // NDEF message TLV
            g_last_ntag_dump.ndef_present = true;
            g_last_ntag_dump.ndef_size = g_last_ntag_dump.pages[4][1];
            g_last_ntag_dump.ndef_start_page = 4;
            printf("   NDEF: NDEF data detected (size: %d bytes)\n", g_last_ntag_dump.ndef_size);
        }
    }
    
    // Display summary
    printf("\nIDENTIFIED: NTAG Dump Complete!\n");
    printf("   INFO: Card Type: %s\n", g_last_ntag_dump.card_type);
    printf("   DETAILS: Pages Read: %d/%d (%.1f%%)\n", 
           g_last_ntag_dump.readable_pages, g_last_ntag_dump.total_pages,
           (float)g_last_ntag_dump.readable_pages / g_last_ntag_dump.total_pages * 100);
    printf("   PROTECTED: Protected Pages: %d\n", g_last_ntag_dump.protected_pages);
    printf("   DATA: Total Data: %d bytes\n", g_last_ntag_dump.readable_pages * 4);
    
    if (g_last_ntag_dump.password_found) {
        printf("   KEY: Password: %08" PRIX32 "\n", g_last_ntag_dump.password);
    }
    
    if (g_last_ntag_dump.ndef_present) {
        printf("   NDEF: NDEF: Present (%d bytes)\n", g_last_ntag_dump.ndef_size);
    }
    
    TERMINAL_VIEW_ADD_TEXT("IDENTIFIED: Dump complete: %d/%d pages\n", 
                           g_last_ntag_dump.readable_pages, g_last_ntag_dump.total_pages);
    
    if (g_last_ntag_dump.password_found) {
        TERMINAL_VIEW_ADD_TEXT("KEY: Password: %08" PRIX32 "\n", g_last_ntag_dump.password);
    }
    
    return g_last_ntag_dump.readable_pages > 0;
}

/**
 * @brief Save comprehensive NTAG dump data to SD card
 * @param filename Custom filename (optional)
 * @return true if saved successfully
 */
bool chameleon_manager_save_ntag_dump(const char* filename) {
    if (!g_last_ntag_dump.valid) {
        printf("ERROR: No NTAG dump data to save\n");
        TERMINAL_VIEW_ADD_TEXT("ERROR: No NTAG dump data to save\n");
        return false;
    }
    
    // Create filename if not provided
    char file_path[256];  // Increased buffer size to avoid truncation
    time_t now = time(NULL);
    
    if (filename == NULL) {
        struct tm* time_info = localtime(&now);
        char uid_str[16] = {0};  // Reduced size to prevent overflow
        for (int i = 0; i < g_last_ntag_dump.uid_size && i < 4; i++) {
            snprintf(uid_str + (i * 2), sizeof(uid_str) - (i * 2), "%02X", g_last_ntag_dump.uid[i]);
        }
        
        snprintf(file_path, sizeof(file_path), 
                "/mnt/ghostesp/chameleon/ntag_%s_%s_%04d%02d%02d_%02d%02d%02d.bin",
                g_last_ntag_dump.card_type,
                uid_str,
                time_info->tm_year + 1900, time_info->tm_mon + 1, time_info->tm_mday,
                time_info->tm_hour, time_info->tm_min, time_info->tm_sec);
    } else {
        if (strstr(filename, ".") == NULL) {
            snprintf(file_path, sizeof(file_path), "/mnt/ghostesp/chameleon/%s.bin", filename);
        } else {
            snprintf(file_path, sizeof(file_path), "/mnt/ghostesp/chameleon/%s", filename);
        }
    }
    
    // Ensure directory exists
    sd_card_create_directory("/mnt/ghostesp/chameleon");
    
    // Create comprehensive dump content
    char content[8192];  // Larger buffer for full dump
    int len = 0;
    
    // Header information
    len += snprintf(content + len, sizeof(content) - len,
        "# Ghost ESP - NTAG Card Dump\n"
        "# ===========================\n"
        "# Generated: %s"
        "# Device: Ghost ESP with Chameleon Ultra\n"
        "# \n"
        "# CARD INFORMATION\n"
        "# ================\n"
        "Card Type: %s\n"
        "UID (%d bytes): ",
        ctime(&now),
        g_last_ntag_dump.card_type,
        g_last_ntag_dump.uid_size);
    
    // Add UID
    for (int i = 0; i < g_last_ntag_dump.uid_size; i++) {
        len += snprintf(content + len, sizeof(content) - len, "%02X", g_last_ntag_dump.uid[i]);
        if (i < g_last_ntag_dump.uid_size - 1) {
            len += snprintf(content + len, sizeof(content) - len, " ");
        }
    }
    
    // Dump statistics
    len += snprintf(content + len, sizeof(content) - len,
        "\nTotal Pages: %d\n"
        "Readable Pages: %d (%.1f%%)\n"
        "Protected Pages: %d\n"
        "Total Data Size: %d bytes\n",
        g_last_ntag_dump.total_pages,
        g_last_ntag_dump.readable_pages,
        (float)g_last_ntag_dump.readable_pages / g_last_ntag_dump.total_pages * 100,
        g_last_ntag_dump.protected_pages,
        g_last_ntag_dump.readable_pages * 4);
    
    // Version information if available
    if (g_last_ntag_dump.version_valid) {
        len += snprintf(content + len, sizeof(content) - len,
            "\n# VERSION INFORMATION\n"
            "# ===================\n"
            "GET_VERSION Response: ");
        for (int i = 0; i < 8; i++) {
            len += snprintf(content + len, sizeof(content) - len, "%02X ", g_last_ntag_dump.version_data[i]);
        }
        len += snprintf(content + len, sizeof(content) - len, "\n");
    }
    
    // Password information
    if (g_last_ntag_dump.password_found) {
        len += snprintf(content + len, sizeof(content) - len,
            "\n# PASSWORD INFORMATION\n"
            "# ====================\n"
            "Password Found: %08" PRIX32 "\n"
            "Authentication: Successful\n",
            g_last_ntag_dump.password);
    } else if (g_last_ntag_dump.password_protected) {
        len += snprintf(content + len, sizeof(content) - len,
            "\n# PASSWORD INFORMATION\n"
            "# ====================\n"
            "Password Protected: Yes\n"
            "Authentication: Failed (default passwords)\n");
    }
    
    // NDEF information
    if (g_last_ntag_dump.ndef_present) {
        len += snprintf(content + len, sizeof(content) - len,
            "\n# NDEF INFORMATION\n"
            "# =================\n"
            "NDEF Present: Yes\n"
            "NDEF Size: %d bytes\n"
            "NDEF Start Page: %d\n",
            g_last_ntag_dump.ndef_size,
            g_last_ntag_dump.ndef_start_page);
    }
    
    // Memory map information
    len += snprintf(content + len, sizeof(content) - len,
        "\n# MEMORY MAP\n"
        "# ===========\n"
        "User Memory Start: Page %d\n"
        "User Memory End: Page %d\n"
        "Lock Bytes Page: %d\n"
        "\n# PAGE DATA\n"
        "# ==========\n",
        g_last_ntag_dump.user_memory_start,
        g_last_ntag_dump.user_memory_end,
        g_last_ntag_dump.lock_bytes_page);
    
    // Page-by-page data
    for (uint16_t page = 0; page < g_last_ntag_dump.total_pages; page++) {
        if (g_last_ntag_dump.page_valid[page]) {
            len += snprintf(content + len, sizeof(content) - len, "Page %03d: ", page);
            for (int i = 0; i < 4; i++) {
                len += snprintf(content + len, sizeof(content) - len, "%02X ", g_last_ntag_dump.pages[page][i]);
            }
            
            // Add ASCII representation
            len += snprintf(content + len, sizeof(content) - len, " |");
            for (int i = 0; i < 4; i++) {
                uint8_t byte = g_last_ntag_dump.pages[page][i];
                if (byte >= 32 && byte <= 126) {
                    len += snprintf(content + len, sizeof(content) - len, "%c", byte);
                } else {
                    len += snprintf(content + len, sizeof(content) - len, ".");
                }
            }
            len += snprintf(content + len, sizeof(content) - len, "|\n");
            
            // Add special page annotations
            if (page == 0) {
                len += snprintf(content + len, sizeof(content) - len, "         # UID and BCC\n");
            } else if (page == 1) {
                len += snprintf(content + len, sizeof(content) - len, "         # UID continuation\n");
            } else if (page == 2) {
                len += snprintf(content + len, sizeof(content) - len, "         # Lock bytes and OTP\n");
            } else if (page == 3) {
                len += snprintf(content + len, sizeof(content) - len, "         # Capability Container (CC)\n");
            } else if (page >= 4 && page <= g_last_ntag_dump.user_memory_end) {
                if (g_last_ntag_dump.ndef_present && page == g_last_ntag_dump.ndef_start_page) {
                    len += snprintf(content + len, sizeof(content) - len, "         # NDEF data start\n");
                } else {
                    len += snprintf(content + len, sizeof(content) - len, "         # User data\n");
                }
            } else if (page > g_last_ntag_dump.user_memory_end) {
                len += snprintf(content + len, sizeof(content) - len, "         # Configuration/Lock pages\n");
            }
        } else {
            len += snprintf(content + len, sizeof(content) - len, "Page %03d: -- -- -- --  # Protected/Unreadable\n", page);
        }
        
        // Prevent buffer overflow
        if (len >= sizeof(content) - 200) {
            len += snprintf(content + len, sizeof(content) - len, "\n# ... (truncated due to size limits)\n");
            break;
        }
    }
    
    // Footer
    len += snprintf(content + len, sizeof(content) - len,
        "\n# ===========================================\n"
        "# End of NTAG dump - Total readable pages: %d\n"
        "# Generated by Ghost ESP - github.com/Spooks4576/Ghost_ESP\n"
        "# ===========================================\n",
        g_last_ntag_dump.readable_pages);
    
    // Write to SD card
    esp_err_t result = sd_card_write_file(file_path, content, len);
    if (result == ESP_OK) {
        printf("DATA: NTAG dump saved to: %s\n", file_path);
        printf("   PAGES: File size: %d bytes\n", len);
        printf("   DETAILS: Contains: %d/%d pages\n", g_last_ntag_dump.readable_pages, g_last_ntag_dump.total_pages);
        
        TERMINAL_VIEW_ADD_TEXT("DATA: Dump saved to: %s\n", file_path);
        TERMINAL_VIEW_ADD_TEXT("DETAILS: %d/%d pages saved\n", g_last_ntag_dump.readable_pages, g_last_ntag_dump.total_pages);
        
        return true;
    } else {
        printf("ERROR: Failed to save NTAG dump\n");
        TERMINAL_VIEW_ADD_TEXT("ERROR: Failed to save dump\n");
        return false;
    }
}

/**
 * @brief Test authentication with a specific key using multiple payload formats
 * This tests the official MF1_AUTH_ONE_KEY_BLOCK command with different payload structures
 * @param block Block number to authenticate to 
 * @param key_type Key type (MF_KEY_A or MF_KEY_B)
 * @param key_hex Hex string representation of the key (12 characters)
 * @return true if authentication succeeded with any format, false otherwise
 */
bool chameleon_manager_test_auth(uint8_t block, uint8_t key_type, const char* key_hex) {
    if (!g_is_connected) {
        printf("Not connected to Chameleon Ultra\n");
        TERMINAL_VIEW_ADD_TEXT("Not connected to Chameleon Ultra\n");
        return false;
    }
    
    // Parse hex key
    uint8_t key[6];
    for (int i = 0; i < 6; i++) {
        char hex_byte[3] = {key_hex[i*2], key_hex[i*2+1], 0};
        key[i] = (uint8_t)strtol(hex_byte, NULL, 16);
    }
    
    printf("Testing MF1_AUTH_ONE_KEY_BLOCK (2007) with multiple payload formats...\n");
    printf("Block: %d, Key Type: %s, Key: %s\n", block, 
           (key_type == MF_KEY_A) ? "A" : "B", key_hex);
    TERMINAL_VIEW_ADD_TEXT("Testing official auth command formats...\n");
    
    // First perform HF scan to make sure card is present
    if (!chameleon_manager_scan_hf()) {
        printf("Failed to scan HF card first\n");
        TERMINAL_VIEW_ADD_TEXT("Failed to scan HF card first\n");
        return false;
    }
    
    // FORMAT 1: [block][key_type][6-byte-key] 
    printf("\n=== Format 1: [block][key_type][key] ===\n");
    uint8_t auth_data_v1[8];
    auth_data_v1[0] = block;
    auth_data_v1[1] = key_type;
    memcpy(&auth_data_v1[2], key, 6);
    
    printf("Payload: %02X %02X %02X%02X%02X%02X%02X%02X\n",
           auth_data_v1[0], auth_data_v1[1], auth_data_v1[2], auth_data_v1[3], 
           auth_data_v1[4], auth_data_v1[5], auth_data_v1[6], auth_data_v1[7]);
    
    memset(&g_last_response, 0, sizeof(g_last_response));
    g_response_received = false;
    
    bool result = send_command(CMD_MF1_AUTH_ONE_KEY_BLOCK, auth_data_v1, 8);
    if (result && xSemaphoreTake(g_response_sem, pdMS_TO_TICKS(3000)) == pdTRUE) {
        if (g_response_received && g_last_response.command == CMD_MF1_AUTH_ONE_KEY_BLOCK) {
            printf("Response: status=0x%02X, data_size=%d\n", 
                   g_last_response.status, g_last_response.data_size);
            
            if (g_last_response.status == STATUS_SUCCESS || g_last_response.status == 0x00) {
                printf("✓ SUCCESS! Format 1 authentication successful\n");
                TERMINAL_VIEW_ADD_TEXT("✓ Format 1 SUCCESS!\n");
                return true;
            } else {
                printf("✗ Format 1 failed: status=0x%02X\n", g_last_response.status);
                if (g_last_response.status == 0x06) {
                    printf("  Command not supported - this suggests firmware incompatibility\n");
                }
            }
        }
    } else {
        printf("✗ Format 1 failed: No response or timeout\n");
    }
    
    // FORMAT 2: [key_type][block][6-byte-key]
    printf("\n=== Format 2: [key_type][block][key] ===\n");
    uint8_t auth_data_v2[8];
    auth_data_v2[0] = key_type;
    auth_data_v2[1] = block;
    memcpy(&auth_data_v2[2], key, 6);
    
    printf("Payload: %02X %02X %02X%02X%02X%02X%02X%02X\n",
           auth_data_v2[0], auth_data_v2[1], auth_data_v2[2], auth_data_v2[3], 
           auth_data_v2[4], auth_data_v2[5], auth_data_v2[6], auth_data_v2[7]);
    
    memset(&g_last_response, 0, sizeof(g_last_response));
    g_response_received = false;
    
    result = send_command(CMD_MF1_AUTH_ONE_KEY_BLOCK, auth_data_v2, 8);
    if (result && xSemaphoreTake(g_response_sem, pdMS_TO_TICKS(3000)) == pdTRUE) {
        if (g_response_received && g_last_response.command == CMD_MF1_AUTH_ONE_KEY_BLOCK) {
            printf("Response: status=0x%02X, data_size=%d\n", 
                   g_last_response.status, g_last_response.data_size);
            
            if (g_last_response.status == STATUS_SUCCESS || g_last_response.status == 0x00) {
                printf("✓ SUCCESS! Format 2 authentication successful\n");
                TERMINAL_VIEW_ADD_TEXT("✓ Format 2 SUCCESS!\n");
                return true;
            } else {
                printf("✗ Format 2 failed: status=0x%02X\n", g_last_response.status);
            }
        }
    } else {
        printf("✗ Format 2 failed: No response or timeout\n");
    }
    
    printf("\nAll MF1_AUTH_ONE_KEY_BLOCK formats failed\n");
    printf("This indicates either:\n");
    printf("1. Wrong key for this card\n");
    printf("2. Firmware doesn't support this command as expected\n");
    printf("3. Different payload format required\n");
    
    TERMINAL_VIEW_ADD_TEXT("All formats failed\n");
    return false;
}

/**
 * @brief Test authentication with both Key A and Key B for a specific block
 * This is a simple wrapper that tests both key types with the same key value
 * @param block Block number to authenticate to 
 * @param key_hex Hex string representation of the key (12 characters)
 * @return true if either authentication succeeded, false otherwise
 */
bool chameleon_manager_test_both_keys(uint8_t block, const char* key_hex) {
    if (!g_is_connected) {
        printf("Not connected to Chameleon Ultra\n");
        TERMINAL_VIEW_ADD_TEXT("Not connected to Chameleon Ultra\n");
        return false;
    }
    
    printf("Testing both Key A and Key B on block %d with key: %s\n", block, key_hex);
    TERMINAL_VIEW_ADD_TEXT("Testing both Key A and Key B...\n");
    
    bool success_a = false;
    bool success_b = false;
    
    // Test Key A
    printf("\n=== Testing Key A ===\n");
    TERMINAL_VIEW_ADD_TEXT("=== Testing Key A ===\n");
    success_a = chameleon_manager_test_auth(block, MF_KEY_A, key_hex);
    
    // Small delay between tests
    vTaskDelay(pdMS_TO_TICKS(200));
    
    // Test Key B  
    printf("\n=== Testing Key B ===\n");
    TERMINAL_VIEW_ADD_TEXT("=== Testing Key B ===\n");
    success_b = chameleon_manager_test_auth(block, MF_KEY_B, key_hex);
    
    // Summary
    printf("\n=== RESULTS ===\n");
    printf("Key A: %s\n", success_a ? "✓ SUCCESS" : "✗ FAILED");
    printf("Key B: %s\n", success_b ? "✓ SUCCESS" : "✗ FAILED");
    TERMINAL_VIEW_ADD_TEXT("=== RESULTS ===\n");
    
    if (success_a || success_b) {
        printf("At least one key type worked!\n");
        TERMINAL_VIEW_ADD_TEXT("At least one key type worked!\n");
        return true;
    } else {
        printf("Both key types failed\n");
        TERMINAL_VIEW_ADD_TEXT("Both key types failed\n");
        return false;
    }
}

/**
 * @brief Enable MFKey32 emulation mode for RF-based key recovery
 * This is the CORRECT approach that generates actual RF activity
 * @return true if MFKey32 mode was enabled successfully
 */
bool chameleon_manager_enable_mfkey32_mode(void) {
    if (!g_is_connected) {
        printf("Not connected to Chameleon Ultra\n");
        TERMINAL_VIEW_ADD_TEXT("Not connected to Chameleon Ultra\n");
        return false;
    }
    
    printf("=== ENABLING MFKey32 EMULATION MODE (with RF activity) ===\n");
    printf("This will:\n");
    printf("1. Switch to emulation mode\n");
    printf("2. Set up MIFARE Classic emulation\n");
    printf("3. Enable MFKey32 nonce collection\n");
    printf("4. Generate actual RF field for key recovery\n\n");
    
    TERMINAL_VIEW_ADD_TEXT("=== ENABLING MFKey32 MODE ===\n");
    
    // Step 1: Switch to emulation mode (generates RF field)
    printf("Step 1: Switching to emulation mode...\n");
    TERMINAL_VIEW_ADD_TEXT("Switching to emulation mode...\n");
    
    uint8_t emulator_mode = 0x00;  // 0x00 = emulator mode, 0x01 = reader mode
    memset(&g_last_response, 0, sizeof(g_last_response));
    g_response_received = false;
    
    bool result = send_command(CMD_CHANGE_DEVICE_MODE, &emulator_mode, 1);
    if (!result || xSemaphoreTake(g_response_sem, pdMS_TO_TICKS(3000)) != pdTRUE) {
        printf("✗ Failed to switch to emulation mode\n");
        TERMINAL_VIEW_ADD_TEXT("✗ Failed to switch to emulation mode\n");
        return false;
    }
    
    if (g_last_response.status != STATUS_SUCCESS) {
        printf("✗ Emulation mode switch failed with status: 0x%02X\n", g_last_response.status);
        TERMINAL_VIEW_ADD_TEXT("✗ Emulation mode switch failed\n");
        return false;
    }
    
    printf("✓ Successfully switched to emulation mode\n");
    TERMINAL_VIEW_ADD_TEXT("✓ Emulation mode enabled\n");
    
    // Step 2: Get the scanned card's UID to set up emulation
    if (!g_last_hf_scan.valid) {
        printf("Step 2: Scanning for HF card to emulate...\n");
        TERMINAL_VIEW_ADD_TEXT("Scanning for card to emulate...\n");
        
        // Switch back to reader mode temporarily
        uint8_t reader_mode = 0x01;
        send_command(CMD_CHANGE_DEVICE_MODE, &reader_mode, 1);
        xSemaphoreTake(g_response_sem, pdMS_TO_TICKS(3000));
        
        if (!chameleon_manager_scan_hf()) {
            printf("✗ No HF card found to emulate\n");
            TERMINAL_VIEW_ADD_TEXT("✗ No card found\n");
            return false;
        }
        
        // Switch back to emulation mode
        send_command(CMD_CHANGE_DEVICE_MODE, &emulator_mode, 1);
        xSemaphoreTake(g_response_sem, pdMS_TO_TICKS(3000));
    }
    
    printf("Step 2: Setting up MIFARE Classic emulation with UID: ");
    for (int i = 0; i < g_last_hf_scan.uid_size; i++) {
        printf("%02X", g_last_hf_scan.uid[i]);
    }
    printf("\n");
    
    // Step 3: Set anticollision data (UID) for emulation
    uint8_t anticoll_data[4];
    memcpy(anticoll_data, g_last_hf_scan.uid, 4);  // Use first 4 bytes of UID
    
    memset(&g_last_response, 0, sizeof(g_last_response));
    g_response_received = false;
    
    result = send_command(CMD_HF14A_SET_ANTI_COLL_DATA, anticoll_data, 4);
    if (result && xSemaphoreTake(g_response_sem, pdMS_TO_TICKS(3000)) == pdTRUE) {
        if (g_last_response.status == STATUS_SUCCESS) {
            printf("✓ Anticollision data set successfully\n");
            TERMINAL_VIEW_ADD_TEXT("✓ UID configured\n");
        } else {
            printf("! Anticollision setup warning: status 0x%02X\n", g_last_response.status);
        }
    } else {
        printf("! Anticollision setup failed\n");
    }
    
    // Step 4: Enable MFKey32 detection/logging
    printf("Step 4: Enabling MFKey32 nonce collection...\n");
    TERMINAL_VIEW_ADD_TEXT("Enabling nonce collection...\n");
    
    uint8_t enable_detection = 0x01;  // Enable detection
    memset(&g_last_response, 0, sizeof(g_last_response));
    g_response_received = false;
    
    result = send_command(CMD_MF1_SET_DETECTION_ENABLE, &enable_detection, 1);
    if (!result || xSemaphoreTake(g_response_sem, pdMS_TO_TICKS(3000)) != pdTRUE) {
        printf("✗ Failed to enable MFKey32 detection\n");
        TERMINAL_VIEW_ADD_TEXT("✗ Detection enable failed\n");
        return false;
    }
    
    if (g_last_response.status != STATUS_SUCCESS) {
        printf("✗ MFKey32 detection enable failed with status: 0x%02X\n", g_last_response.status);
        TERMINAL_VIEW_ADD_TEXT("✗ Detection enable failed\n");
        return false;
    }
    
    printf("✓ MFKey32 nonce collection enabled\n");
    TERMINAL_VIEW_ADD_TEXT("✓ Nonce collection enabled\n");
    
    printf("\nIDENTIFIED: SUCCESS! Chameleon Ultra is now in MFKey32 emulation mode\n");
    printf("📡 The device is generating RF field and ready for key recovery\n");
    printf("\nINFO: NEXT STEPS:\n");
    printf("1. Present the Chameleon Ultra to your card reader\n");
    printf("2. The reader will attempt authentication (generating nonces)\n");
    printf("3. Use 'chameleon collectnonces' to retrieve collected data\n");
    printf("4. RF activity should be visible now!\n\n");
    
    TERMINAL_VIEW_ADD_TEXT("✓ MFKey32 mode enabled - RF active!\n");
    TERMINAL_VIEW_ADD_TEXT("Present device to reader now\n");
    
    return true;
}

/**
 * @brief Collect nonces from MFKey32 emulation mode
 * @return true if nonces were collected successfully
 */
bool chameleon_manager_collect_nonces(void) {
    if (!g_is_connected) {
        printf("Not connected to Chameleon Ultra\n");
        TERMINAL_VIEW_ADD_TEXT("Not connected to Chameleon Ultra\n");
        return false;
    }
    
    printf("=== COLLECTING MFKey32 NONCES ===\n");
    TERMINAL_VIEW_ADD_TEXT("Collecting nonces...\n");
    
    // Step 1: Check detection count
    memset(&g_last_response, 0, sizeof(g_last_response));
    g_response_received = false;
    
    bool result = send_command(CMD_MF1_GET_DETECTION_COUNT, NULL, 0);
    if (!result || xSemaphoreTake(g_response_sem, pdMS_TO_TICKS(3000)) != pdTRUE) {
        printf("Failed to get detection count\n");
        TERMINAL_VIEW_ADD_TEXT("Failed to get detection count\n");
        return false;
    }
    
    if (g_last_response.status != STATUS_SUCCESS) {
        printf("Get detection count failed with status: 0x%02X\n", g_last_response.status);
        TERMINAL_VIEW_ADD_TEXT("Detection count failed\n");
        return false;
    }
    
    if (g_last_response.data_size < 4) {
        printf("Invalid detection count response size: %d\n", g_last_response.data_size);
        TERMINAL_VIEW_ADD_TEXT("Invalid count response\n");
        return false;
    }
    
    uint32_t detection_count = (g_last_response.data[0] << 24) | 
                              (g_last_response.data[1] << 16) |
                              (g_last_response.data[2] << 8) | 
                               g_last_response.data[3];
    
    printf("Detection count: %" PRIu32 " nonces collected\n", detection_count);
    TERMINAL_VIEW_ADD_TEXT("Detected: %" PRIu32 " nonces\n", detection_count);
    
    if (detection_count == 0) {
        printf("\nWARNING:  No nonces collected yet!\n");
        printf("Make sure to:\n");
        printf("1. Present the Chameleon Ultra to your card reader\n");
        printf("2. The reader should attempt to authenticate to the card\n");
        printf("3. Failed authentications generate nonces for key recovery\n");
        TERMINAL_VIEW_ADD_TEXT("WARNING: No nonces collected yet\n");
        return false;
    }
    
    // Step 2: Get detection log (actual nonces)
    memset(&g_last_response, 0, sizeof(g_last_response));
    g_response_received = false;
    
    result = send_command(CMD_MF1_GET_DETECTION_LOG, NULL, 0);
    if (!result || xSemaphoreTake(g_response_sem, pdMS_TO_TICKS(5000)) != pdTRUE) {
        printf("Failed to get detection log\n");
        TERMINAL_VIEW_ADD_TEXT("Failed to get detection log\n");
        return false;
    }
    
    if (g_last_response.status != STATUS_SUCCESS) {
        printf("Get detection log failed with status: 0x%02X\n", g_last_response.status);
        TERMINAL_VIEW_ADD_TEXT("Detection log failed\n");
        return false;
    }
    
    printf("✓ Retrieved %d bytes of nonce data\n", g_last_response.data_size);
    TERMINAL_VIEW_ADD_TEXT("✓ Nonce data retrieved\n");
    
    // Display collected nonces for analysis
    printf("\nDETAILS: NONCE DATA FOR KEY RECOVERY:\n");
    printf("Raw data (%d bytes): ", g_last_response.data_size);
    for (int i = 0; i < g_last_response.data_size && i < 64; i++) {
        printf("%02X ", g_last_response.data[i]);
        if ((i + 1) % 16 == 0) printf("\n");
    }
    if (g_last_response.data_size > 64) {
        printf("... (truncated)\n");
    }
    printf("\n");
    
    printf("FOUND: SUCCESS! RF-based key recovery data collected\n");
    printf("INFO: Use this nonce data with offline tools like:\n");
    printf("   - mfcuk (Darkside attack)\n");
    printf("   - mfoc (nested attack)\n");
    printf("   - Proxmark3 tools\n\n");
    
    TERMINAL_VIEW_ADD_TEXT("✓ Key recovery data ready!\n");
    
    return true;
}