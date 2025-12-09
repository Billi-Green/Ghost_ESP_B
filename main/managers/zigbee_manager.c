// Ensure sdkconfig is visible before checking target macros
#include "sdkconfig.h"
#if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6)
#include "managers/zigbee_manager.h"
#include "managers/status_display_manager.h"
#include "esp_ieee802154.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "esp_log.h"
#include <string.h>
#include "vendor/pcap.h"

#define ZB_MAX_FRAME_LEN 127
#define ZB_QUEUE_LEN 32

static const char *TAG = "ZBMAN";
static QueueHandle_t s_frame_q;
static TaskHandle_t s_task;
static volatile bool s_capturing = false;
static bool s_hop = false;
static uint8_t s_cur_ch = 15;
static TimerHandle_t s_hop_timer = NULL;
static bool s_filter_zigbee_only = true;

#define ZB_MAX_DEVICES 64
static zigbee_device_t s_devices[ZB_MAX_DEVICES];
static int s_device_count = 0;

typedef struct {
    uint8_t data[ZB_MAX_FRAME_LEN];
    uint8_t len;
    int8_t rssi;
    uint8_t channel;
} zb_frame_t;

static void add_device(const uint8_t *addr, uint8_t addr_len, int8_t rssi, uint8_t channel) {
    for (int i = 0; i < s_device_count; i++) {
        if (s_devices[i].addr_len == addr_len && memcmp(s_devices[i].addr, addr, addr_len) == 0) {
            if (rssi > s_devices[i].rssi) s_devices[i].rssi = rssi;
            return;
        }
    }
    if (s_device_count < ZB_MAX_DEVICES) {
        memcpy(s_devices[s_device_count].addr, addr, addr_len);
        s_devices[s_device_count].addr_len = addr_len;
        s_devices[s_device_count].rssi = rssi;
        s_devices[s_device_count].channel = channel;
        s_device_count++;
    }
}

static void parse_src_addr(const uint8_t *frame, uint8_t len, int8_t rssi, uint8_t channel) {
    if (len < 3) return;
    uint16_t fcf = (uint16_t)frame[0] | ((uint16_t)frame[1] << 8);
    uint8_t dest_mode = (fcf >> 10) & 0x3;
    uint8_t src_mode = (fcf >> 14) & 0x3;
    bool pan_comp = (fcf >> 6) & 0x1;
    
    int idx = 3;
    if (dest_mode) {
        idx += 2;
        idx += (dest_mode == 2) ? 2 : (dest_mode == 3 ? 8 : 0);
    }
    if (src_mode) {
        if (!pan_comp) idx += 2;
        int slen = (src_mode == 2) ? 2 : (src_mode == 3 ? 8 : 0);
        if (slen > 0 && idx + slen <= len) {
            add_device(&frame[idx], slen, rssi, channel);
        }
    }
}

static void zigbee_capture_task(void *arg) {
    zb_frame_t item;
    while (s_capturing) {
        if (xQueueReceive(s_frame_q, &item, pdMS_TO_TICKS(100)) == pdTRUE) {
            pcap_write_packet_to_buffer(item.data, item.len, PCAP_CAPTURE_IEEE802154);
            parse_src_addr(item.data, item.len, item.rssi, item.channel);
        }
    }
    s_task = NULL;
    vTaskDelete(NULL);
}

static void hop_timer_cb(TimerHandle_t xTimer) {
    if (!s_capturing || !s_hop) return;
    // Cycle channels 11..26
    if (s_cur_ch < 11 || s_cur_ch > 26) s_cur_ch = 11;
    else {
        s_cur_ch++;
        if (s_cur_ch > 26) s_cur_ch = 11;
    }
    esp_ieee802154_set_channel(s_cur_ch);
    esp_ieee802154_receive();
}

/* // Minimal 802.15.4 MAC header parsing to locate payload start
static int mac_payload_offset(const uint8_t *p, uint8_t len) {
    if (len < 3) return -1;
    uint16_t fcf = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
    int idx = 3; // FCF(2) + Seq(1)

    uint8_t dest_mode = (fcf >> 10) & 0x3;
    uint8_t frame_ver = (fcf >> 12) & 0x3; // not used, but kept for clarity
    uint8_t src_mode  = (fcf >> 14) & 0x3;
    bool pan_comp = (fcf >> 6) & 0x1;
    bool sec_en = (fcf >> 3) & 0x1;

    if (dest_mode) {
        if (len < idx + 2) return -1; // Dest PAN
        idx += 2;
        int dlen = (dest_mode == 2) ? 2 : (dest_mode == 3 ? 8 : 0);
        if (len < idx + dlen) return -1;
        idx += dlen;
    }
    if (src_mode) {
        if (!pan_comp) {
            if (len < idx + 2) return -1; // Src PAN
            idx += 2;
        }
        int slen = (src_mode == 2) ? 2 : (src_mode == 3 ? 8 : 0);
        if (len < idx + slen) return -1;
        idx += slen;
    }

    if (sec_en) {
        if (len < idx + 5) return -1; // Security Control(1) + Frame Counter(4)
        uint8_t sc = p[idx];
        idx += 5;
        uint8_t key_mode = (sc >> 3) & 0x3;
        int key_len = (key_mode == 0) ? 0 : (key_mode == 1 ? 1 : (key_mode == 2 ? 5 : 9));
        if (len < idx + key_len) return -1;
        idx += key_len;
    }

    return (idx <= len) ? idx : -1;
} */

/* // Heuristic to detect 6LoWPAN/Thread payloads
static inline bool is_6lowpan_dispatch(uint8_t b) {
    if (b == 0x41) return true;                 // LOWPAN_IPV6 (uncompressed IPv6)
    if ((b & 0xE0) == 0x60) return true;        // LOWPAN_IPHC (0x60-0x7F)
    if ((b & 0xF8) == 0xC0) return true;        // FRAG1
    if ((b & 0xF8) == 0xE0) return true;        // FRAGN
    if ((b & 0xC0) == 0x80) return true;        // Mesh/Broadcast headers (0x80-0xBF)
    return false;
}
 */
esp_err_t zigbee_manager_start_capture(uint8_t channel) {
    if (s_capturing) return ESP_OK;

    if (s_frame_q == NULL) {
        s_frame_q = xQueueCreate(ZB_QUEUE_LEN, sizeof(zb_frame_t));
        if (!s_frame_q) return ESP_ERR_NO_MEM;
    }

    // channel == 0 means enable channel hopping
    s_hop = (channel == 0);
    if (!s_hop) {
        if (channel < 11 || channel > 26) channel = 15;
        s_cur_ch = channel;
    } else {
        s_cur_ch = 11; // start of hop range
    }

    esp_err_t err = esp_ieee802154_enable();
    if (err != ESP_OK) return err;

    esp_ieee802154_set_promiscuous(true);
    esp_ieee802154_set_rx_when_idle(true);
    esp_ieee802154_set_channel(s_cur_ch);

    s_capturing = true;
    xTaskCreate(zigbee_capture_task, "zb_cap", 4096, NULL, 5, &s_task);

    // Ensure RX is active immediately
    esp_ieee802154_receive();

    // Start hopping timer if requested (200 ms dwell)
    if (s_hop) {
        if (!s_hop_timer) {
            s_hop_timer = xTimerCreate("zb_hop", pdMS_TO_TICKS(200), pdTRUE, NULL, hop_timer_cb);
        }
        if (s_hop_timer) xTimerStart(s_hop_timer, 0);
    }
    status_display_show_status("Zigbee Started");
    return ESP_OK;
}

void zigbee_manager_stop_capture(void) {
    if (!s_capturing) return;
    s_capturing = false;
    s_hop = false;
    if (s_hop_timer) {
        xTimerStop(s_hop_timer, portMAX_DELAY);
        xTimerDelete(s_hop_timer, portMAX_DELAY);
        s_hop_timer = NULL;
    }
    esp_ieee802154_set_rx_when_idle(false);
    esp_ieee802154_set_promiscuous(false);
    esp_ieee802154_disable();
    for (int i = 0; i < 10 && s_task != NULL; ++i) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (s_frame_q) {
        vQueueDelete(s_frame_q);
        s_frame_q = NULL;
    }
    status_display_show_status("Zigbee Stopped");
}

bool zigbee_manager_is_capturing(void) { return s_capturing; }

// ISR context callback from IEEE802.15.4 driver
void IRAM_ATTR esp_ieee802154_receive_done(uint8_t *frame, esp_ieee802154_frame_info_t *frame_info) {
    if (!s_capturing || !s_frame_q || !frame) {
        esp_ieee802154_receive_handle_done(frame);
        return;
    }
    uint8_t len = frame[0];
    if (len > ZB_MAX_FRAME_LEN) len = ZB_MAX_FRAME_LEN;

    zb_frame_t item;
    item.len = len;
    item.rssi = frame_info ? frame_info->rssi : -100;
    item.channel = s_cur_ch;
    memcpy(item.data, frame + 1, len);

/*     // Optional Zigbee-only filtering: drop likely Thread/6LoWPAN frames
    if (s_filter_zigbee_only) {
        int off = mac_payload_offset(item.data, item.len);
        if (off >= 0 && off < item.len) {
            uint8_t disp = item.data[off];
            if (is_6lowpan_dispatch(disp)) {
                esp_ieee802154_receive_handle_done(frame);
                esp_ieee802154_receive();
                return;
            }
        }
    }
 */
    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(s_frame_q, &item, &woken);
    esp_ieee802154_receive_handle_done(frame);
    // Continue RX immediately; yield (if needed) as the very last step
    esp_ieee802154_receive();
    if (woken == pdTRUE) portYIELD_FROM_ISR();
}

void IRAM_ATTR esp_ieee802154_receive_sfd_done(void) {}
void IRAM_ATTR esp_ieee802154_transmit_done(const uint8_t *frame, const uint8_t *ack, esp_ieee802154_frame_info_t *ack_frame_info) {}
void IRAM_ATTR esp_ieee802154_transmit_failed(const uint8_t *frame, esp_ieee802154_tx_error_t error) {}
void IRAM_ATTR esp_ieee802154_transmit_sfd_done(uint8_t *frame) {}
void IRAM_ATTR esp_ieee802154_energy_detect_done(int8_t power) {}
void IRAM_ATTR esp_ieee802154_receive_at_done(void) {}

void zigbee_manager_set_filter_zigbee_only(bool enable) { s_filter_zigbee_only = enable; }

void zigbee_manager_clear_devices(void) {
    s_device_count = 0;
}

int zigbee_manager_get_device_count(void) {
    return s_device_count;
}

int zigbee_manager_get_device_data(int index, zigbee_device_t *out) {
    if (index < 0 || index >= s_device_count || !out) return -1;
    *out = s_devices[index];
    return 0;
}

#endif
