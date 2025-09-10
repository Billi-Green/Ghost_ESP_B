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
#include "host/ble_sm.h"
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

// PIN support
char g_chameleon_pin[7] = {0}; // Store PIN as string (max 6 digits + null terminator)
bool g_pin_required = false;

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

// NTAG default passwords removed in simplified version

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


// Key types for authentication
#define MF_KEY_A 0x60
#define MF_KEY_B 0x61

// Enhanced card dump with recovered keys
typedef struct {
    uint8_t key[6];
    bool valid;
} sector_key_t;

typedef struct {
    sector_key_t key_a;
    sector_key_t key_b;
    bool auth_success_a;
    bool auth_success_b;
} sector_auth_t;


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
static int chameleon_sm_io_cb(uint16_t conn_handle, const struct ble_sm_io *io, void *arg);

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

bool chameleon_manager_connect(uint32_t timeout_seconds, const char* pin) {
    if (!g_is_initialized) {
        chameleon_manager_init();
    }
    
    if (g_is_connected) {
        printf("Already connected to Chameleon Ultra\n");
        TERMINAL_VIEW_ADD_TEXT("Already connected to Chameleon Ultra\n");
        return true;
    }
    
    // Handle PIN parameter
    if (pin != NULL && strlen(pin) > 0) {
        // Validate PIN (should be 4-6 digits)
        int pin_len = strlen(pin);
        if (pin_len < 4 || pin_len > 6) {
            printf("Invalid PIN: must be 4-6 digits\n");
            TERMINAL_VIEW_ADD_TEXT("Invalid PIN: must be 4-6 digits\n");
            return false;
        }
        
        // Check if PIN contains only digits
        for (int i = 0; i < pin_len; i++) {
            if (pin[i] < '0' || pin[i] > '9') {
                printf("Invalid PIN: must contain only digits\n");
                TERMINAL_VIEW_ADD_TEXT("Invalid PIN: must contain only digits\n");
                return false;
            }
        }
        
        // Store PIN for authentication
        strncpy(g_chameleon_pin, pin, sizeof(g_chameleon_pin) - 1);
        g_chameleon_pin[sizeof(g_chameleon_pin) - 1] = '\0';
        g_pin_required = true;
        
        printf("PIN set for Chameleon Ultra authentication\n");
        TERMINAL_VIEW_ADD_TEXT("PIN set for authentication\n");
    } else {
        // Clear PIN if not provided
        memset(g_chameleon_pin, 0, sizeof(g_chameleon_pin));
        g_pin_required = false;
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
                        
                        // Identify card type based on ATQA and SAK
                        uint16_t atqa = (atqa_hi << 8) | atqa_lo;
                        if (atqa == 0x0044 && sak == 0x00) {
                            // This is likely an NTAG card - ATQA 0x0044 and SAK 0x00 are NTAG characteristics
                            snprintf(g_last_hf_scan.tag_type, sizeof(g_last_hf_scan.tag_type), 
                                    "NTAG (ATQA:%02X%02X SAK:%02X)", atqa_hi, atqa_lo, sak);
                        } else {
                            snprintf(g_last_hf_scan.tag_type, sizeof(g_last_hf_scan.tag_type), 
                                    "HF-14A (ATQA:%02X%02X SAK:%02X)", atqa_hi, atqa_lo, sak);
                        }
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




bool chameleon_manager_read_hf_card(void) {
    if (!g_is_connected) {
        printf("Not connected to Chameleon Ultra\n");
        TERMINAL_VIEW_ADD_TEXT("Not connected to Chameleon Ultra\n");
        return false;
    }
    
    printf("Starting basic HF card reading...\n");
    TERMINAL_VIEW_ADD_TEXT("Starting basic HF card reading...\n");
    
    // First scan to get card info
    if (!chameleon_manager_scan_hf()) {
        printf("Failed to scan HF card first\n");
        TERMINAL_VIEW_ADD_TEXT("Failed to scan HF card first\n");
        return false;
    }
    
    // Initialize data structures
    memset(&g_last_card_dump, 0, sizeof(g_last_card_dump));
    g_last_card_dump.valid = true;
    g_last_card_dump.timestamp = time(NULL);
    
    // Copy basic info from scan
    if (g_last_hf_scan.valid) {
        memcpy(g_last_card_dump.uid, g_last_hf_scan.uid, g_last_hf_scan.uid_size);
        g_last_card_dump.uid_size = g_last_hf_scan.uid_size;
        strncpy(g_last_card_dump.tag_type, g_last_hf_scan.tag_type, sizeof(g_last_card_dump.tag_type));
        
        printf("Card detected: %s\n", g_last_card_dump.tag_type);
        printf("UID: ");
        for (int i = 0; i < g_last_card_dump.uid_size; i++) {
            printf("%02X", g_last_card_dump.uid[i]);
                            }
                            printf("\n");
        TERMINAL_VIEW_ADD_TEXT("Card detected: %s\n", g_last_card_dump.tag_type);
    }
    
    printf("Basic card information collected successfully.\n");
    printf("Note: For detailed MIFARE Classic analysis, use specialized tools.\n");
    TERMINAL_VIEW_ADD_TEXT("Basic card reading completed\n");
    
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
    
    // Create basic dump content
    char content[1024];
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
        "\nNote: Basic card information only. For detailed analysis, use specialized tools.\n");
    
    // Write to file
    if (sd_card_write_file(file_path, content, len)) {
        printf("Card dump saved to: %s\n", file_path);
        TERMINAL_VIEW_ADD_TEXT("Card dump saved successfully\n");
        return true;
    } else {
        printf("Failed to save card dump to: %s\n", file_path);
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
    
    // Create scan report content
    char content[1024];
    int len = snprintf(content, sizeof(content),
        "Chameleon Ultra HF Scan Report\n"
        "==============================\n"
        "Timestamp: %s"
        "Tag Type: %s\n"
        "UID Size: %d bytes\n"
        "UID: ",
        ctime(&g_last_hf_scan.timestamp),
        g_last_hf_scan.tag_type,
        g_last_hf_scan.uid_size);
    
    // Add UID
    for (int i = 0; i < g_last_hf_scan.uid_size && i < 20; i++) {
        len += snprintf(content + len, sizeof(content) - len, "%02X ", g_last_hf_scan.uid[i]);
    }
    
    len += snprintf(content + len, sizeof(content) - len, 
        "\nNote: Basic scan information only. For detailed analysis, use specialized tools.\n");
    
    // Write to file
    if (sd_card_write_file(file_path, content, len)) {
        printf("HF scan saved to: %s\n", file_path);
        TERMINAL_VIEW_ADD_TEXT("HF scan saved successfully\n");
        return true;
    } else {
        printf("Failed to save HF scan to: %s\n", file_path);
        TERMINAL_VIEW_ADD_TEXT("Failed to save HF scan\n");
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
    
    // Create scan report content
    char content[1024];
    int len = snprintf(content, sizeof(content),
        "Chameleon Ultra LF Scan Report\n"
        "==============================\n"
        "Timestamp: %s"
        "Tag Type: %s\n"
        "UID Size: %d bytes\n"
        "UID: ",
        ctime(&g_last_lf_scan.timestamp),
        g_last_lf_scan.tag_type,
        g_last_lf_scan.uid_size);
    
    // Add UID
    for (int i = 0; i < g_last_lf_scan.uid_size && i < 20; i++) {
        len += snprintf(content + len, sizeof(content) - len, "%02X ", g_last_lf_scan.uid[i]);
    }
    
    len += snprintf(content + len, sizeof(content) - len, 
        "\nNote: Basic scan information only. For detailed analysis, use specialized tools.\n");
    
    // Write to file
    if (sd_card_write_file(file_path, content, len)) {
        printf("LF scan saved to: %s\n", file_path);
        TERMINAL_VIEW_ADD_TEXT("LF scan saved successfully\n");
        return true;
    } else {
        printf("Failed to save LF scan to: %s\n", file_path);
        TERMINAL_VIEW_ADD_TEXT("Failed to save LF scan\n");
        return false;
    }
}

bool chameleon_manager_detect_ntag(void) {
    if (!g_is_connected) {
        printf("Not connected to Chameleon Ultra\n");
        TERMINAL_VIEW_ADD_TEXT("Not connected to Chameleon Ultra\n");
        return false;
    }
    
    printf("Detecting NTAG card...\n");
    TERMINAL_VIEW_ADD_TEXT("Detecting NTAG card...\n");
    
    // First, scan for HF cards to detect NTAG
    if (!chameleon_manager_scan_hf()) {
        printf("No HF card detected\n");
        TERMINAL_VIEW_ADD_TEXT("No HF card detected\n");
        return false;
    }
    
    // Check if the detected card is an NTAG by looking at tag type
    if (strstr(g_last_hf_scan.tag_type, "NTAG") != NULL) {
        printf("NTAG card detected!\n");
        printf("UID: ");
        for (int i = 0; i < g_last_hf_scan.uid_size; i++) {
            printf("%02X", g_last_hf_scan.uid[i]);
        }
        printf("\n");
        
        // Try to get NTAG version to determine exact type
        printf("Reading NTAG version...\n");
        TERMINAL_VIEW_ADD_TEXT("Reading NTAG version...\n");
        
        // Use HF14A_RAW to send the NTAG GET_VERSION command to the physical card
        uint8_t version_cmd[1] = {0x60}; // NTAG GET_VERSION command
        
        g_response_received = false;
        
        if (send_command(CMD_HF14A_RAW, version_cmd, 1)) {
            if (xSemaphoreTake(g_response_sem, pdMS_TO_TICKS(3000)) == pdTRUE) {
                if (g_response_received && g_last_response.command == CMD_HF14A_RAW && 
                    g_last_response.status == STATUS_SUCCESS && g_last_response.data_size >= 4) {
                    
                    uint8_t vendor_id = g_last_response.data[0];
                    uint8_t product_type = g_last_response.data[1];
                    uint8_t product_subtype = g_last_response.data[2];
                    uint8_t major_version = g_last_response.data[3];
                    uint8_t minor_version = g_last_response.data[4];
                    
                    printf("Version: %02X %02X %02X %02X %02X\n", 
                           vendor_id, product_type, product_subtype, major_version, minor_version);
                    
                    // Determine NTAG type based on version data
                    char ntag_type[32];
                    int total_pages = 45; // Default to NTAG213
                    
                    if (vendor_id == 0x04 && product_type == 0x04) {
                        if (product_subtype == 0x02) {
                            strcpy(ntag_type, "NTAG213");
                            total_pages = NTAG213_TOTAL_PAGES;
                        } else if (product_subtype == 0x0E) {
                            strcpy(ntag_type, "NTAG215");
                            total_pages = NTAG215_TOTAL_PAGES;
                        } else if (product_subtype == 0x0F) {
                            strcpy(ntag_type, "NTAG216");
                            total_pages = NTAG216_TOTAL_PAGES;
                        } else {
                            snprintf(ntag_type, sizeof(ntag_type), "NTAG (Unknown subtype: %02X)", product_subtype);
                        }
                    } else {
                        snprintf(ntag_type, sizeof(ntag_type), "NTAG (Unknown: %02X %02X)", vendor_id, product_type);
                    }
                    
                    printf("Card Type: %s\n", ntag_type);
                    printf("Memory Size: %d pages (%d bytes)\n", total_pages, total_pages * NTAG_PAGE_SIZE);
                    
                    // Update the stored tag type
                    strncpy(g_last_hf_scan.tag_type, ntag_type, sizeof(g_last_hf_scan.tag_type) - 1);
                    
                    TERMINAL_VIEW_ADD_TEXT("NTAG card detected!\n");
                    TERMINAL_VIEW_ADD_TEXT("Card Type: ");
                    TERMINAL_VIEW_ADD_TEXT(ntag_type);
                    TERMINAL_VIEW_ADD_TEXT("\n");
                    
                    return true;
                } else {
                    printf("Failed to read NTAG version, using basic detection\n");
                    TERMINAL_VIEW_ADD_TEXT("Failed to read NTAG version, using basic detection\n");
                }
            } else {
                printf("Timeout reading NTAG version, using basic detection\n");
                TERMINAL_VIEW_ADD_TEXT("Timeout reading NTAG version, using basic detection\n");
            }
        } else {
            printf("Failed to send version command, using basic detection\n");
            TERMINAL_VIEW_ADD_TEXT("Failed to send version command, using basic detection\n");
        }
        
        // Fallback to basic detection - estimate based on UID length
        int estimated_pages = 45; // Default to NTAG213
        char estimated_type[32] = "NTAG (Unknown)";
        
        if (g_last_hf_scan.uid_size == 4) {
            estimated_pages = 45; // NTAG213
            strcpy(estimated_type, "NTAG213 (estimated)");
        } else if (g_last_hf_scan.uid_size == 7) {
            estimated_pages = 135; // NTAG215
            strcpy(estimated_type, "NTAG215 (estimated)");
        } else {
            estimated_pages = 135; // Default to NTAG215 for other lengths
            strcpy(estimated_type, "NTAG (estimated)");
        }
        
        printf("Card Type: %s\n", estimated_type);
        printf("Memory Size: %d pages (%d bytes) - estimated\n", estimated_pages, estimated_pages * NTAG_PAGE_SIZE);
        
        // Update the stored tag type with our estimate
        strncpy(g_last_hf_scan.tag_type, estimated_type, sizeof(g_last_hf_scan.tag_type) - 1);
        
        TERMINAL_VIEW_ADD_TEXT("NTAG card detected!\n");
        TERMINAL_VIEW_ADD_TEXT("Card Type: ");
        TERMINAL_VIEW_ADD_TEXT(estimated_type);
        TERMINAL_VIEW_ADD_TEXT("\n");
        
        return true;
    } else {
        printf("Card detected but not an NTAG\n");
        printf("Card type: %s\n", g_last_hf_scan.tag_type);
        TERMINAL_VIEW_ADD_TEXT("Card detected but not an NTAG\n");
        TERMINAL_VIEW_ADD_TEXT("Card type: ");
        TERMINAL_VIEW_ADD_TEXT(g_last_hf_scan.tag_type);
        TERMINAL_VIEW_ADD_TEXT("\n");
        return false;
    }
}
    
bool chameleon_manager_ntag_authenticate(uint32_t password) {
    if (!g_is_connected) {
        printf("Not connected to Chameleon Ultra\n");
        TERMINAL_VIEW_ADD_TEXT("Not connected to Chameleon Ultra\n");
        return false;
    }
    
    // NTAG authentication not implemented in simplified version
    printf("NTAG authentication not implemented in simplified version\n");
    TERMINAL_VIEW_ADD_TEXT("NTAG authentication not implemented\n");
        return false;
    }
    
bool chameleon_manager_read_ntag_card(void) {
    if (!g_is_connected) {
        printf("Not connected to Chameleon Ultra\n");
        TERMINAL_VIEW_ADD_TEXT("Not connected to Chameleon Ultra\n");
            return false;
        }
        
    // NTAG reading not implemented in simplified version
    printf("NTAG reading not implemented in simplified version\n");
    TERMINAL_VIEW_ADD_TEXT("NTAG reading not implemented\n");
            return false;
}

bool chameleon_manager_save_ntag_dump(const char* filename) {
    if (!g_last_ntag_dump.valid) {
        printf("No NTAG dump data to save\n");
        TERMINAL_VIEW_ADD_TEXT("No NTAG dump data to save\n");
        return false;
    }
    
    // NTAG dump saving not implemented in simplified version
    printf("NTAG dump saving not implemented in simplified version\n");
    TERMINAL_VIEW_ADD_TEXT("NTAG dump saving not implemented\n");
        return false;
}

bool chameleon_manager_test_auth(uint8_t block, uint8_t key_type, const char* key_hex) {
    if (!g_is_connected) {
        printf("Not connected to Chameleon Ultra\n");
        TERMINAL_VIEW_ADD_TEXT("Not connected to Chameleon Ultra\n");
        return false;
    }
    
    // Authentication testing not implemented in simplified version
    printf("Authentication testing not implemented in simplified version\n");
    TERMINAL_VIEW_ADD_TEXT("Authentication testing not implemented\n");
        return false;
    }
    
bool chameleon_manager_test_both_keys(uint8_t block, const char* key_hex) {
    if (!g_is_connected) {
        printf("Not connected to Chameleon Ultra\n");
        TERMINAL_VIEW_ADD_TEXT("Not connected to Chameleon Ultra\n");
        return false;
    }
    
    // Both keys testing not implemented in simplified version
    printf("Both keys testing not implemented in simplified version\n");
    TERMINAL_VIEW_ADD_TEXT("Both keys testing not implemented\n");
        return false;
}

bool chameleon_manager_enable_mfkey32_mode(void) {
    if (!g_is_connected) {
        printf("Not connected to Chameleon Ultra\n");
        TERMINAL_VIEW_ADD_TEXT("Not connected to Chameleon Ultra\n");
        return false;
    }
    
    // MFKey32 mode not implemented in simplified version
    printf("MFKey32 mode not implemented in simplified version\n");
    TERMINAL_VIEW_ADD_TEXT("MFKey32 mode not implemented\n");
        return false;
    }
    
bool chameleon_manager_collect_nonces(void) {
    if (!g_is_connected) {
        printf("Not connected to Chameleon Ultra\n");
        TERMINAL_VIEW_ADD_TEXT("Not connected to Chameleon Ultra\n");
        return false;
    }
    
    // Nonce collection not implemented in simplified version
    printf("Nonce collection not implemented in simplified version\n");
    TERMINAL_VIEW_ADD_TEXT("Nonce collection not implemented\n");
        return false;
    }
    