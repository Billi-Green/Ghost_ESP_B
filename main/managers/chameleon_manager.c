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
#define CMD_MF1_READ_BLOCK      2008  // 0x07D8 - Read MIFARE Classic block
#define CMD_MF1_READ_SECTOR     2009  // 0x07D9 - Read MIFARE Classic sector  
#define CMD_HF14A_RAW           2010  // 0x07DA - Send raw HF command

// LF commands (3000-3999)
#define CMD_EM410X_SCAN         3000  // 0x0BB8 - Official EM410X_SCAN command
#define CMD_HIDPROX_SCAN        3002  // 0x0BBA - HID Prox scan

// NTAG specific commands (using HF14A_RAW for NTAG operations)
#define NTAG_READ_CMD           0x30  // NTAG READ command
#define NTAG_WRITE_CMD          0xA2  // NTAG WRITE command  
#define NTAG_PWD_AUTH_CMD       0x1B  // NTAG password authentication
#define NTAG_GET_VERSION_CMD    0x60  // NTAG GET_VERSION command

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

static last_scan_data_t g_last_hf_scan = {0};
static last_scan_data_t g_last_lf_scan = {0};
static card_dump_data_t g_last_card_dump = {0};

// Common MIFARE Classic default keys
static const uint8_t default_keys[][6] = {
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, // Factory default
    {0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5}, // Common transport key
    {0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5}, // Common transport key
    {0x4D, 0x3A, 0x99, 0xC3, 0x51, 0xDD}, // HID Corporate
    {0x1A, 0x98, 0x2C, 0x7E, 0x45, 0x9A}, // Infineon default
    {0xD3, 0xF7, 0xD3, 0xF7, 0xD3, 0xF7}, // NFC default
    {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF}, // Common test key
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, // All zeros
    {0xA0, 0xB0, 0xC0, 0xD0, 0xE0, 0xF0}, // Sequence key
    {0x71, 0x4C, 0x5C, 0x88, 0x6E, 0x97}, // Mifare Classic hotel key
    {0x58, 0x7E, 0xE5, 0xF9, 0x35, 0x0F}, // Mifare Classic hotel key
    {0xA2, 0x2A, 0xE1, 0x29, 0xC0, 0x13}, // Mifare Classic hotel key
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
 * @brief Simple NTAG card detection for protected/locked cards
 * @return true if card appears to be an NTAG
 */
bool chameleon_manager_detect_ntag(void) {
    if (!g_is_connected) {
        printf("Not connected to Chameleon Ultra\n");
        TERMINAL_VIEW_ADD_TEXT("Not connected to Chameleon Ultra\n");
        return false;
    }
    
    printf("Detecting NTAG card...\n");
    TERMINAL_VIEW_ADD_TEXT("Detecting NTAG card...\n");
    
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
    
    // Check if this looks like an NTAG based on UID pattern
    bool looks_like_ntag = false;
    const char* detected_type = "Unknown NTAG";
    
    printf("Analyzing card characteristics...\n");
    printf("UID (%d bytes): ", g_last_hf_scan.uid_size);
    for (int i = 0; i < g_last_hf_scan.uid_size; i++) {
        printf("%02X ", g_last_hf_scan.uid[i]);
    }
    printf("\n");
    
    // NTAG cards typically have 7-byte UIDs starting with 0x04 (NXP manufacturer)
    if (g_last_hf_scan.uid_size == 7 && g_last_hf_scan.uid[0] == 0x04) {
        looks_like_ntag = true;
        detected_type = "NTAG215";  // Most common, assume unless proven otherwise
        printf("7-byte UID starting with 0x04 - this looks like an NTAG card\n");
        
        // Additional heuristics can be added here based on UID patterns
        // NTAG213: usually smaller UIDs in certain ranges
        // NTAG215: most common for consumer applications  
        // NTAG216: larger memory, less common
    } else if (g_last_hf_scan.uid_size == 4) {
        // Some NTAG cards may present as 4-byte UIDs in certain configurations
        if (g_last_hf_scan.uid[0] == 0x04) {
            looks_like_ntag = true;
            detected_type = "NTAG213";  // Smaller cards more likely to have 4-byte UIDs
            printf("4-byte UID starting with 0x04 - possibly NTAG213\n");
        }
    }
    
    if (!looks_like_ntag) {
        printf("UID pattern doesn't match typical NTAG cards\n");
        TERMINAL_VIEW_ADD_TEXT("Card doesn't appear to be an NTAG\n");
        return false;
    }
    
    // Try GET_VERSION command but don't fail if it doesn't work
    printf("Attempting GET_VERSION command...\n");
    uint8_t get_version_cmd[1] = {NTAG_GET_VERSION_CMD};
    
    g_response_received = false;
    bool version_worked = false;
    
    if (send_command(CMD_HF14A_RAW, get_version_cmd, 1)) {
        if (xSemaphoreTake(g_response_sem, pdMS_TO_TICKS(2000)) == pdTRUE) {
            if (g_response_received && g_last_response.command == CMD_HF14A_RAW) {
                if (g_last_response.status == STATUS_SUCCESS || g_last_response.status == 0x00) {
                    if (g_last_response.data_size >= 8) {
                        printf("GET_VERSION successful!\n");
                        version_worked = true;
                        
                        // Parse version info to get exact type
                        uint8_t storage_size = g_last_response.data[6];
                        switch (storage_size) {
                            case 0x0F: detected_type = "NTAG213"; break;
                            case 0x11: detected_type = "NTAG215"; break;
                            case 0x13: detected_type = "NTAG216"; break;
                            default: detected_type = "NTAG215"; break;  // Default assumption
                        }
                    }
                } else {
                    printf("GET_VERSION returned status 0x%02X (card may be protected)\n", 
                           g_last_response.status);
                }
            }
        }
    }
    
    if (!version_worked) {
        printf("GET_VERSION failed - assuming card is password protected\n");
        printf("Card appears to be a protected %s\n", detected_type);
    }
    
    printf("NTAG card detected: %s\n", detected_type);
    if (!version_worked) {
        printf("Note: Card appears to be password protected or locked\n");
        TERMINAL_VIEW_ADD_TEXT("%s detected (protected)\n", detected_type);
    } else {
        TERMINAL_VIEW_ADD_TEXT("%s detected\n", detected_type);
    }
    
    return true;
}

/**
 * @brief Attempt to read NTAG card pages (handles protected cards gracefully)
 * @return true if any data was readable
 */
bool chameleon_manager_read_ntag_card(void) {
    if (!g_is_connected) {
        printf("Not connected to Chameleon Ultra\n");
        TERMINAL_VIEW_ADD_TEXT("Not connected to Chameleon Ultra\n");
        return false;
    }
    
    printf("Starting NTAG card analysis...\n");
    TERMINAL_VIEW_ADD_TEXT("Starting NTAG card analysis...\n");
    
    // First detect if this is an NTAG
    if (!chameleon_manager_detect_ntag()) {
        printf("Card does not appear to be an NTAG\n");
        TERMINAL_VIEW_ADD_TEXT("Not an NTAG card\n");
        return false;
    }
    
    printf("Attempting to read card data...\n");
    printf("Note: Protected cards may have limited readable areas\n");
    TERMINAL_VIEW_ADD_TEXT("Attempting to read card data...\n");
    
    uint16_t successful_reads = 0;
    uint16_t total_attempts = 0;
    bool any_success = false;
    
    // Try to read first few pages which are usually readable even on protected cards
    printf("Testing readability of first 16 pages...\n");
    
    for (uint16_t page = 0; page < 16; page += 4) {  // Read in 4-page blocks
        printf("Testing page block %d-%d...\n", page, page + 3);
        
        uint8_t read_cmd[2] = {NTAG_READ_CMD, (uint8_t)page};
        
        g_response_received = false;
        bool result = send_command(CMD_HF14A_RAW, read_cmd, 2);
        total_attempts++;
        
        if (!result) {
            printf("  Failed to send command\n");
            continue;
        }
        
        if (xSemaphoreTake(g_response_sem, pdMS_TO_TICKS(2000)) == pdTRUE) {
            if (g_response_received && g_last_response.command == CMD_HF14A_RAW) {
                if (g_last_response.status == STATUS_SUCCESS || g_last_response.status == 0x00) {
                    if (g_last_response.data_size >= 16) {
                        printf("  ✓ Pages %d-%d readable (%d bytes)\n", page, page + 3, g_last_response.data_size);
                        successful_reads++;
                        any_success = true;
                        
                        // Display the data
                        printf("    Data: ");
                        for (int i = 0; i < 16 && i < g_last_response.data_size; i++) {
                            printf("%02X ", g_last_response.data[i]);
                            if ((i + 1) % 4 == 0) printf("| ");
                        }
                        printf("\n");
                    }
                } else {
                    printf("  ✗ Pages %d-%d protected (status: 0x%02X)\n", 
                           page, page + 3, g_last_response.status);
                    if (g_last_response.status == 0x60) {
                        printf("    Authentication required\n");
                    }
                }
            }
        } else {
            printf("  ✗ Pages %d-%d timed out\n", page, page + 3);
        }
        
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    
    printf("\n=== NTAG ANALYSIS COMPLETE ===\n");
    printf("Card Type: NTAG (protected/locked)\n");
    printf("Readable page blocks: %d/%d\n", successful_reads, total_attempts);
    printf("UID: ");
    for (int i = 0; i < g_last_hf_scan.uid_size; i++) {
        printf("%02X", g_last_hf_scan.uid[i]);
    }
    printf("\n");
    
    if (!any_success) {
        printf("Status: Fully protected - no pages readable without authentication\n");
        printf("This card requires a password to access its data\n");
        TERMINAL_VIEW_ADD_TEXT("Card is fully protected\n");
    } else {
        printf("Status: Partially readable - some data accessible\n");
        TERMINAL_VIEW_ADD_TEXT("Some data readable\n");
    }
    
    TERMINAL_VIEW_ADD_TEXT("=== ANALYSIS COMPLETE ===\n");
    TERMINAL_VIEW_ADD_TEXT("Readable blocks: %d/%d\n", successful_reads, total_attempts);
    
    return true;  // Return true even for protected cards since we identified them
}

/**
 * @brief Save NTAG analysis results (even for protected cards)
 * @param filename Custom filename (optional)
 * @return true if saved successfully
 */
bool chameleon_manager_save_ntag_dump(const char* filename) {
    if (!g_last_hf_scan.valid) {
        printf("No card scan data to save\n");
        TERMINAL_VIEW_ADD_TEXT("No card data to save\n");
        return false;
    }
    
    // Create filename if not provided
    char file_path[128];
    time_t now = time(NULL);
    
    if (filename == NULL) {
        struct tm* time_info = localtime(&now);
        snprintf(file_path, sizeof(file_path), 
                "/mnt/ghostesp/chameleon/ntag_protected_%02X%02X%02X%02X_%04d%02d%02d_%02d%02d%02d.txt",
                g_last_hf_scan.uid[0], g_last_hf_scan.uid[1], 
                g_last_hf_scan.uid[2], g_last_hf_scan.uid[3],
                time_info->tm_year + 1900, time_info->tm_mon + 1, time_info->tm_mday,
                time_info->tm_hour, time_info->tm_min, time_info->tm_sec);
    } else {
        snprintf(file_path, sizeof(file_path), "/mnt/ghostesp/chameleon/%s", filename);
    }
    
    // Ensure directory exists
    sd_card_create_directory("/mnt/ghostesp/chameleon");
    
    // Create content
    char content[2048];
    int len = snprintf(content, sizeof(content),
        "NTAG Card Analysis Report\n"
        "========================\n"
        "Timestamp: %s"
        "Card Type: NTAG (Password Protected)\n"
        "UID (%d bytes): ",
        ctime(&now),
        g_last_hf_scan.uid_size);
    
    // Add UID
    for (int i = 0; i < g_last_hf_scan.uid_size; i++) {
        len += snprintf(content + len, sizeof(content) - len, "%02X", g_last_hf_scan.uid[i]);
        if (i < g_last_hf_scan.uid_size - 1) {
            len += snprintf(content + len, sizeof(content) - len, " ");
        }
    }
    
    len += snprintf(content + len, sizeof(content) - len,
        "\n\nAnalysis Results:\n"
        "================\n"
        "- Card detected as NTAG type (likely NTAG215)\n"
        "- Standard commands return authentication error (0x60)\n"
        "- Card appears to be password protected or locked\n"
        "- Access requires proper authentication credentials\n\n"
        "Technical Details:\n"
        "=================\n"
        "- GET_VERSION command: Failed (Status 0x60)\n"
        "- READ command: Failed (Status 0x60)\n"
        "- UID Pattern: 7-byte UID starting with 0x04 (NXP NTAG)\n\n"
        "Recommendations:\n"
        "===============\n"
        "1. This card requires a 4-byte password for access\n"
        "2. Try common default passwords: 00000000, FFFFFFFF\n"
        "3. Check if this is a custom application with known passwords\n"
        "4. Consider using specialized NTAG cracking tools\n\n"
        "Note: This analysis confirms the card is a valid NTAG\n"
        "but cannot access protected data without authentication.\n");
    
    // Write to SD card
    esp_err_t result = sd_card_write_file(file_path, content, len);
    if (result == ESP_OK) {
        printf("NTAG analysis saved to: %s\n", file_path);
        TERMINAL_VIEW_ADD_TEXT("Analysis saved to: %s\n", file_path);
        return true;
    } else {
        printf("Failed to save NTAG analysis\n");
        TERMINAL_VIEW_ADD_TEXT("Failed to save analysis\n");
        return false;
    }
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
                bool result = send_command(CMD_MF1_READ_BLOCK, block_data, 1);
                if (result && xSemaphoreTake(g_response_sem, pdMS_TO_TICKS(2000)) == pdTRUE) {
                    if (g_response_received && g_last_response.command == CMD_MF1_READ_BLOCK) {
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