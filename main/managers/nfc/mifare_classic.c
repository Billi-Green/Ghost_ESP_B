#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "managers/nfc/mifare_classic.h"
#include "managers/sd_card_manager.h"
#include "esp_log.h"

static const uint8_t DEFAULT_KEYS[][6] = {
    {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF},
    {0xA0,0xA1,0xA2,0xA3,0xA4,0xA5},
    {0x00,0x00,0x00,0x00,0x00,0x00},
};

// Forward declarations for helpers defined later
static esp_err_t mfc_auth_block(pn532_io_handle_t io, uint8_t block, bool use_key_b,
                                const uint8_t key[6], const uint8_t *uid, uint8_t uid_len);
static esp_err_t mfc_read_block(pn532_io_handle_t io, uint8_t block, uint8_t out16[16]);

bool mfc_is_classic_sak(uint8_t sak) {
    return (sak == 0x08) || (sak == 0x18) || (sak == 0x09);
}

bool mfc_save_flipper_file(pn532_io_handle_t io,
                           const uint8_t* uid,
                           uint8_t uid_len,
                           uint16_t atqa,
                           uint8_t sak,
                           const char* out_dir,
                           char* out_path,
                           size_t out_path_len) {
    if (!io || !uid || uid_len == 0 || !out_dir) return false;
    sd_card_create_directory(out_dir);

    // Build filename: Classic<Type>_<UID>.nfc
    char uid_part[40] = {0};
    int up = 0;
    for (uint8_t i = 0; i < uid_len && up < (int)sizeof(uid_part) - 3; ++i) {
        up += snprintf(uid_part + up, sizeof(uid_part) - up, "%02X", uid[i]);
        if (i + 1 < uid_len) up += snprintf(uid_part + up, sizeof(uid_part) - up, "-");
    }
    char path[192];
    const char *mtype = "Classic";
    switch (mfc_type_from_sak(sak)) {
        case MFC_MINI: mtype = "ClassicMini"; break;
        case MFC_1K:   mtype = "Classic1K"; break;
        case MFC_4K:   mtype = "Classic4K"; break;
        default:       mtype = "Classic"; break;
    }
    snprintf(path, sizeof(path), "%s/%s_%s.nfc", out_dir, mtype, uid_part);
    if (out_path && out_path_len) snprintf(out_path, out_path_len, "%s", path);

    MFC_TYPE t = mfc_type_from_sak(sak);
    int sectors = mfc_sector_count(t);
    // Header
    char buf[256]; int pos = 0;
    pos += snprintf(buf + pos, sizeof(buf) - pos, "Filetype: Flipper NFC device\n");
    pos += snprintf(buf + pos, sizeof(buf) - pos, "Version: 4\n");
    pos += snprintf(buf + pos, sizeof(buf) - pos, "Device type: Mifare Classic\n");
    pos += snprintf(buf + pos, sizeof(buf) - pos, "UID:");
    for (uint8_t i = 0; i < uid_len && pos < (int)sizeof(buf) - 4; ++i)
        pos += snprintf(buf + pos, sizeof(buf) - pos, " %02X", uid[i]);
    pos += snprintf(buf + pos, sizeof(buf) - pos, "\n");
    pos += snprintf(buf + pos, sizeof(buf) - pos, "ATQA: %02X %02X\n", (atqa>>8)&0xFF, atqa&0xFF);
    pos += snprintf(buf + pos, sizeof(buf) - pos, "SAK: %02X\n", sak);
    const char *tstr = (t==MFC_4K?"4K":(t==MFC_1K?"1K":(t==MFC_MINI?"Mini":"Unknown")));
    pos += snprintf(buf + pos, sizeof(buf) - pos, "Mifare Classic type: %s\n", tstr);
    pos += snprintf(buf + pos, sizeof(buf) - pos, "Data format version: 2\n");
    if (sd_card_write_file(path, buf, (size_t)pos) != ESP_OK) {
        ESP_LOGE("MFC", "Header write failed: %s", path);
        return false;
    }

    // Dump blocks sector-by-sector
    for (int s = 0; s < sectors; ++s) {
        int first = mfc_first_block_of_sector(t, s);
        int blocks = mfc_blocks_in_sector(t, s);
        bool authed = false;
        // Try key A then key B with defaults
        for (int k = 0; k < (int)(sizeof(DEFAULT_KEYS)/6) && !authed; ++k) {
            if (mfc_auth_block(io, (uint8_t)first, false, DEFAULT_KEYS[k], uid, uid_len) == ESP_OK)
                authed = true;
        }
        for (int k = 0; k < (int)(sizeof(DEFAULT_KEYS)/6) && !authed; ++k) {
            if (mfc_auth_block(io, (uint8_t)first, true, DEFAULT_KEYS[k], uid, uid_len) == ESP_OK)
                authed = true;
        }
        for (int b = 0; b < blocks; ++b) {
            uint8_t data[16]; bool known = false;
            if (authed) {
                if (mfc_read_block(io, (uint8_t)(first + b), data) == ESP_OK) known = true;
            }
            pos = 0;
            pos += snprintf(buf + pos, sizeof(buf) - pos, "Block %d:", first + b);
            if (known) {
                for (int i = 0; i < 16 && pos < (int)sizeof(buf) - 4; ++i)
                    pos += snprintf(buf + pos, sizeof(buf) - pos, " %02X", data[i]);
            } else {
                for (int i = 0; i < 16 && pos < (int)sizeof(buf) - 4; ++i)
                    pos += snprintf(buf + pos, sizeof(buf) - pos, " ??");
            }
            pos += snprintf(buf + pos, sizeof(buf) - pos, "\n");
            sd_card_append_file(path, buf, (size_t)pos);
        }
    }
    return true;
}

MFC_TYPE mfc_type_from_sak(uint8_t sak) {
    if (sak == 0x18) return MFC_4K;
    if (sak == 0x08) return MFC_1K;
    if (sak == 0x09) return MFC_MINI;
    return MFC_UNKNOWN;
}

int mfc_sector_count(MFC_TYPE t) {
    switch (t) {
        case MFC_MINI: return 5;   // 320B
        case MFC_1K:  return 16;  // 1KB
        case MFC_4K:  return 40;  // 4KB
        default: return 0;
    }
}

int mfc_blocks_in_sector(MFC_TYPE t, int sector) {
    if (t == MFC_4K && sector >= 32) return 16;
    return 4;
}

int mfc_first_block_of_sector(MFC_TYPE t, int sector) {
    if (t == MFC_4K) {
        if (sector < 32) return sector * 4;
        return 32 * 4 + (sector - 32) * 16; // 128 + (s-32)*16
    }
    return sector * 4;
}

static esp_err_t mfc_auth_block(pn532_io_handle_t io, uint8_t block, bool use_key_b,
                                const uint8_t key[6], const uint8_t *uid, uint8_t uid_len) {
    if (!io || !uid || uid_len < 4) return ESP_ERR_INVALID_ARG;
    uint8_t cmd[12];
    cmd[0] = use_key_b ? MIFARE_CMD_AUTH_B : MIFARE_CMD_AUTH_A;
    cmd[1] = block;
    memcpy(&cmd[2], key, 6);
    // Use last 4 bytes of UID
    memcpy(&cmd[8], &uid[uid_len - 4], 4);
    uint8_t resp[2] = {0};
    uint8_t resp_len = sizeof(resp);
    return pn532_in_data_exchange(io, cmd, sizeof(cmd), resp, &resp_len);
}

static esp_err_t mfc_read_block(pn532_io_handle_t io, uint8_t block, uint8_t out16[16]) {
    uint8_t cmd[2] = { MIFARE_CMD_READ, block };
    uint8_t resp_len = 16;
    return pn532_in_data_exchange(io, cmd, sizeof(cmd), out16, &resp_len);
}

static const char* mfc_type_str(MFC_TYPE t) {
    switch (t) {
        case MFC_MINI: return "MIFARE Classic Mini";
        case MFC_1K: return "MIFARE Classic 1K";
        case MFC_4K: return "MIFARE Classic 4K";
        default: return "MIFARE Classic";
    }
}

char* mfc_build_details_summary(pn532_io_handle_t io,
                                const uint8_t* uid,
                                uint8_t uid_len,
                                uint16_t atqa,
                                uint8_t sak) {
    MFC_TYPE t = mfc_type_from_sak(sak);
    int sectors = mfc_sector_count(t);
    if (sectors == 0) sectors = 16; // fallback

    // Try auth first-block of each sector with common keys
    int readable = 0, a_cnt = 0, b_cnt = 0;
    // Collect compact per-sector result into a small buffer
    char sec_buf[256]; int spos = 0; sec_buf[0] = '\0';

    for (int s = 0; s < sectors; ++s) {
        int first = mfc_first_block_of_sector(t, s);
        bool ok = false;
        // Try Key A then Key B with a few defaults
        for (int k = 0; k < (int)(sizeof(DEFAULT_KEYS)/6) && !ok; ++k) {
            if (mfc_auth_block(io, (uint8_t)first, false, DEFAULT_KEYS[k], uid, uid_len) == ESP_OK) {
                readable++; a_cnt++; ok = true;
                if (spos < (int)sizeof(sec_buf) - 6) spos += snprintf(sec_buf + spos, sizeof(sec_buf) - spos, "%dA ", s);
                break;
            }
        }
        if (ok) continue;
        for (int k = 0; k < (int)(sizeof(DEFAULT_KEYS)/6) && !ok; ++k) {
            if (mfc_auth_block(io, (uint8_t)first, true, DEFAULT_KEYS[k], uid, uid_len) == ESP_OK) {
                readable++; b_cnt++; ok = true;
                if (spos < (int)sizeof(sec_buf) - 6) spos += snprintf(sec_buf + spos, sizeof(sec_buf) - spos, "%dB ", s);
                break;
            }
        }
        (void)mfc_read_block; // silence unused if we expand later
    }

    size_t cap = 1024; char *out = (char*)malloc(cap); if (!out) return NULL;
    char *w = out; size_t rem = cap; int n = 0;
    n = snprintf(w, rem, "Card: %s | UID:", mfc_type_str(t)); w += n; rem -= n;
    for (uint8_t i = 0; i < uid_len && rem > 4; ++i) { n = snprintf(w, rem, " %02X", uid[i]); w += n; rem -= n; }
    n = snprintf(w, rem, "\nATQA: %02X %02X | SAK: %02X\n", (atqa>>8)&0xFF, atqa&0xFF, sak); w+=n; rem-=n;
    n = snprintf(w, rem, "Sectors: %d, Readable: %d (A:%d, B:%d)\n", sectors, readable, a_cnt, b_cnt); w+=n; rem-=n;
    if (spos > 0) {
        // Trim trailing space
        if (sec_buf[spos-1] == ' ') sec_buf[spos-1] = '\0';
        n = snprintf(w, rem, "Keys: %s\n", sec_buf);
        (void)n;
    }
    return out;
}
