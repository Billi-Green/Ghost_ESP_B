// aerial_detector_manager.h
//
// opendroneid message decoding portions derived from:
// opendroneid-core-c (https://github.com/opendroneid/opendroneid-core-c)
// Licensed under Apache License 2.0
//
// detection approach informed by:
// nyanBOX by jbohack (https://github.com/jbohack/nyanBOX)
// Copyright (c) 2025 jbohack, Licensed under MIT
//
#ifndef AERIAL_DETECTOR_MANAGER_H
#define AERIAL_DETECTOR_MANAGER_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_wifi_types.h"

#ifdef CONFIG_SPIRAM
#define AERIAL_MAX_DEVICES 16
#else
#define AERIAL_MAX_DEVICES 8
#endif
#define AERIAL_MAC_STR_LEN 18
#define AERIAL_ID_MAX_LEN 32
#define AERIAL_DESC_MAX_LEN 64
#define AERIAL_VENDOR_MAX_LEN 32

typedef enum {
    AERIAL_TYPE_UNKNOWN = 0,
    AERIAL_TYPE_REMOTEID_WIFI,
    AERIAL_TYPE_REMOTEID_BLE,
    AERIAL_TYPE_DJI_WIFI,
    AERIAL_TYPE_DJI_BLE,
    AERIAL_TYPE_DRONE_NETWORK,
    AERIAL_TYPE_RC_CONTROLLER,
    AERIAL_TYPE_FPV_VIDEO,
    AERIAL_TYPE_TELEMETRY_MAV,
    AERIAL_TYPE_TELEMETRY_SPORT,
    AERIAL_TYPE_TELEMETRY_CRSF,
} AerialDeviceType;

typedef enum {
    AERIAL_STATUS_UNKNOWN = 0,
    AERIAL_STATUS_GROUND,
    AERIAL_STATUS_AIRBORNE,
    AERIAL_STATUS_EMERGENCY,
    AERIAL_STATUS_SYSTEM_FAILURE,
} AerialStatus;

typedef struct {
    char mac[AERIAL_MAC_STR_LEN];
    char device_id[AERIAL_ID_MAX_LEN];
    char description[AERIAL_DESC_MAX_LEN];
    char vendor[AERIAL_VENDOR_MAX_LEN];
    char operator_id[AERIAL_ID_MAX_LEN];
    
    AerialDeviceType type;
    AerialStatus status;
    int8_t rssi;
    uint8_t channel;
    
    double latitude;
    double longitude;
    float altitude;
    float speed_horizontal;
    float direction;
    float height_agl;
    
    double operator_latitude;
    double operator_longitude;
    float operator_altitude;
    
    uint8_t aircraft_type;
    uint32_t messages_seen;
    uint32_t last_seen_ms;
    
    bool has_location;
    bool has_operator_location;
    bool is_tracked;
} AerialDevice;

typedef void (*AerialDetectorCallback)(AerialDevice *device, void *user_data);

void aerial_detector_init(void);
void aerial_detector_deinit(void);

esp_err_t aerial_detector_start_scan(uint32_t duration_ms);
esp_err_t aerial_detector_stop_scan(void);
bool aerial_detector_is_scanning(void);

int aerial_detector_get_device_count(void);
AerialDevice* aerial_detector_get_device(int index);
AerialDevice* aerial_detector_find_device_by_mac(const char *mac);

void aerial_detector_clear_devices(void);
void aerial_detector_remove_old_devices(uint32_t max_age_ms);
void aerial_detector_compact_known_devices(void);

void aerial_detector_set_callback(AerialDetectorCallback callback, void *user_data);

const char* aerial_detector_get_type_string(AerialDeviceType type);
const char* aerial_detector_get_status_string(AerialStatus status);

esp_err_t aerial_detector_track_device(const char *mac);
esp_err_t aerial_detector_untrack_device(void);
AerialDevice* aerial_detector_get_tracked_device(void);

void aerial_detector_enable_opendroneid(bool enable);
void aerial_detector_enable_dji_detection(bool enable);
void aerial_detector_enable_network_detection(bool enable);
void aerial_detector_enable_telemetry_detection(bool enable);

// channel hopping configuration
void aerial_detector_set_channel_hop_interval(uint32_t interval_ms);
uint32_t aerial_detector_get_channel_hop_interval(void);
int aerial_detector_get_channel_count(void);
void aerial_detector_get_channels(uint8_t *channels, int max_count);

// drone emulation/spoofing
esp_err_t aerial_detector_start_emulation(const char *device_id, double lat, double lon, float alt);
esp_err_t aerial_detector_stop_emulation(void);
bool aerial_detector_is_emulating(void);
void aerial_detector_update_emulation_position(double lat, double lon, float alt);

#endif
