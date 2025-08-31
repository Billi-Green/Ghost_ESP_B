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
#include <stdio.h>
#include <string.h>

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
#define CMD_HF14A_RAW           2010  // 0x07DA - Send raw HF command

// LF commands (3000-3999)
#define CMD_EM410X_SCAN         3000  // 0x0BB8 - Official EM410X_SCAN command
#define CMD_HIDPROX_SCAN        3002  // 0x0BBA - HID Prox scan

// Mode constants
#define HW_MODE_READER          0x01
#define HW_MODE_EMULATOR        0x00

// Status codes
#define STATUS_SUCCESS          0x68
#define STATUS_HF_TAG_OK        0x00
#define STATUS_HF_TAG_NO        0x01
#define STATUS_LF_TAG_OK        0x00
#define STATUS_LF_TAG_NO        0x01

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
    
    bool result = send_command(CMD_EM410X_SCAN, NULL, 0);
    if (result) {
        printf("LF scan command sent, waiting for response...\n");
        TERMINAL_VIEW_ADD_TEXT("LF scan command sent, waiting for response...\n");
        
        // Wait for response
        if (xSemaphoreTake(g_response_sem, pdMS_TO_TICKS(10000)) == pdTRUE) {
            if (g_response_received && g_last_response.command == CMD_EM410X_SCAN) {
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
                        
                        return true;
                    }
                } else if (g_last_response.status == 0x41) { // EM410X_TAG_NO_FOUND
                    printf("No LF tag found\n");
                    TERMINAL_VIEW_ADD_TEXT("No LF tag found\n");
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
                uint8_t slot = g_last_response.data[0];
                printf("Active Slot: %d\n", slot);
                TERMINAL_VIEW_ADD_TEXT("Active Slot: %d\n", slot);
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
    
    printf("Setting active slot to %d...\n", slot);
    TERMINAL_VIEW_ADD_TEXT("Setting active slot to %d...\n", slot);
    
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
                printf("Active slot set to %d successfully\n", slot);
                TERMINAL_VIEW_ADD_TEXT("Active slot set to %d successfully\n", slot);
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
    
    printf("Getting slot %d information...\n", slot);
    TERMINAL_VIEW_ADD_TEXT("Getting slot %d information...\n", slot);
    
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
                
                printf("Slot %d Info:\n", slot);
                printf("  HF Type: 0x%02X\n", hf_type);
                printf("  LF Type: 0x%02X\n", lf_type);
                printf("  Enabled: %s\n", enabled ? "Yes" : "No");
                
                TERMINAL_VIEW_ADD_TEXT("Slot %d Info:\n", slot);
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
            if (g_last_response.status == STATUS_SUCCESS && g_last_response.data_size >= 1) {
                uint8_t support = g_last_response.data[0];
                const char* support_str = support ? "Supported" : "Not Supported";
                printf("MIFARE Classic Support: %s\n", support_str);
                TERMINAL_VIEW_ADD_TEXT("MIFARE Classic Support: %s\n", support_str);
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
            if (g_last_response.status == STATUS_SUCCESS && g_last_response.data_size >= 1) {
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