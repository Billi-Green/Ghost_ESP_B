#ifndef SCREEN_MIRROR_H
#define SCREEN_MIRROR_H

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

#define MIRROR_MARKER 0x47455350  // "GESP"
#define MIRROR_END_MARKER 0x444E4547  // "GEND"
#define MIRROR_CMD_INFO 0x01
#define MIRROR_CMD_FRAME 0x02
#define MIRROR_CMD_FRAME_RLE 0x03
#define MIRROR_CMD_FRAME_8BIT 0x04
#define MIRROR_CMD_FRAME_8BIT_RLE 0x05
// Packed RGB444 (12-bit) raw frame: 2 pixels -> 3 bytes, last odd pixel -> 2 bytes
#define MIRROR_CMD_FRAME_12BIT 0x06

typedef struct __attribute__((packed)) {
    uint32_t marker;
    uint8_t cmd;
    uint16_t x1;
    uint16_t y1;
    uint16_t x2;
    uint16_t y2;
    uint32_t data_len;
} mirror_packet_header_t;

void screen_mirror_init(void);
void screen_mirror_set_enabled(bool enabled);
bool screen_mirror_is_enabled(void);
void screen_mirror_send_area(const lv_area_t *area, lv_color_t *color_p);
void screen_mirror_send_info(void);
void screen_mirror_refresh(void);

#endif // SCREEN_MIRROR_H
