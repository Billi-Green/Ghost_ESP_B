#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include "managers/nfc/ndef.h"

static size_t append_str(char **p, size_t *cap, const char *s) {
    size_t l = strlen(s);
    if (l + 1 > *cap) return 0;
    memcpy(*p, s, l);
    *p += l;
    *cap -= l;
    **p = '\0';
    return l;
}

static size_t append_fmt(char **p, size_t *cap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(*p, *cap, fmt, ap);
    va_end(ap);
    if (n <= 0) return 0;
    size_t used = (size_t)n;
    if (used >= *cap) used = *cap ? *cap - 1 : 0;
    *p += used;
    *cap -= used;
    return used;
}

static const char* ndef_uri_prefix[] = {
    [0x00] = NULL,
    [0x01] = "http://www.",
    [0x02] = "https://www.",
    [0x03] = "http://",
    [0x04] = "https://",
    [0x05] = "tel:",
    [0x06] = "mailto:",
    [0x07] = "ftp://anonymous:anonymous@",
    [0x08] = "ftp://ftp.",
    [0x09] = "ftps://",
    [0x0A] = "sftp://",
    [0x0B] = "smb://",
    [0x0C] = "nfs://",
    [0x0D] = "ftp://",
    [0x0E] = "dav://",
    [0x0F] = "news:",
    [0x10] = "telnet://",
    [0x11] = "imap:",
    [0x12] = "rtsp://",
    [0x13] = "urn:",
    [0x14] = "pop:",
    [0x15] = "sip:",
    [0x16] = "sips:",
    [0x17] = "tftp:",
    [0x18] = "btspp://",
    [0x19] = "btl2cap://",
    [0x1A] = "btgoep://",
    [0x1B] = "tcpobex://",
    [0x1C] = "irdaobex://",
    [0x1D] = "file://",
    [0x1E] = "urn:epc:id:",
    [0x1F] = "urn:epc:tag:",
    [0x20] = "urn:epc:pat:",
    [0x21] = "urn:epc:raw:",
    [0x22] = "urn:epc:",
    [0x23] = "urn:nfc:",
};

static char lowerc(char c) { return (c >= 'A' && c <= 'Z') ? (c + 32) : c; }
static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}
static void append_percent_decoded(const char *s, size_t len, char **out, size_t *cap) {
    size_t i = 0;
    while (i < len && *cap > 1) {
        char c = s[i++];
        if (c == '%' && i + 1 < len) {
            int h1 = hexval(s[i]);
            int h2 = (i + 1 < len) ? hexval(s[i + 1]) : -1;
            if (h1 >= 0 && h2 >= 0) {
                char d = (char)((h1 << 4) | h2);
                **out = d; (*out)++; (*cap)--; i += 2; continue;
            }
        } else if (c == '+') {
            **out = ' '; (*out)++; (*cap)--; continue;
        }
        **out = c; (*out)++; (*cap)--;
    }
}

static void parse_ndef_record(uint8_t tnf,
                              const uint8_t *type,
                              uint8_t type_len,
                              const uint8_t *payload,
                              size_t payload_len,
                              char **out,
                              size_t *cap) {
    if (tnf == 0x01 && type_len == 1 && type[0] == 'U' && payload_len >= 1) {
        uint8_t code = payload[0];
        const char *pre = (code < (sizeof(ndef_uri_prefix)/sizeof(ndef_uri_prefix[0]))) ? ndef_uri_prefix[code] : NULL;
        const char *scheme = pre;
        const char *rest = (const char *)(payload + 1);
        size_t rest_len = (payload_len > 1) ? (payload_len - 1) : 0;
        if (!scheme && rest_len >= 4 && lowerc(rest[0])=='s' && lowerc(rest[1])=='m' && lowerc(rest[2])=='s' && rest[3]==':') {
            append_str(out, cap, "SMS ");
            const char *num = rest + 4;
            const char *q = memchr(num, '?', rest_len - 4);
            size_t num_len = q ? (size_t)(q - num) : (size_t)rest_len - 4;
            append_percent_decoded(num, num_len, out, cap);
            if (q) {
                const char *params = q + 1;
                size_t params_len = (size_t)((rest + rest_len) - params);
                const char *bodyk = NULL;
                // bounded search for "body=" inside params
                for (size_t i = 0; i + 5 <= params_len; ++i) {
                    if (params[i] == 'b' && i + 5 <= params_len &&
                        params[i+1] == 'o' && params[i+2] == 'd' && params[i+3] == 'y' && params[i+4] == '=') {
                        bodyk = params + i + 5;
                        break;
                    }
                }
                if (bodyk) {
                    const char *params_end = rest + rest_len;
                    const char *amp = memchr(bodyk, '&', (size_t)(params_end - bodyk));
                    size_t blen = amp ? (size_t)(amp - bodyk) : (size_t)(params_end - bodyk);
                    append_str(out, cap, " - ");
                    append_percent_decoded(bodyk, blen, out, cap);
                }
            }
            append_str(out, cap, "\n");
            return;
        }
        if (!scheme && rest_len >= 6 && lowerc(rest[0])=='s' && lowerc(rest[1])=='m' && lowerc(rest[2])=='s' && lowerc(rest[3])=='t' && lowerc(rest[4])=='o' && rest[5]==':') {
            append_str(out, cap, "SMS ");
            const char *p = rest + 6;
            size_t p_len = rest_len - 6;
            const char *col = memchr(p, ':', p_len);
            if (col) {
                append_percent_decoded(p, (size_t)(col - p), out, cap);
                append_str(out, cap, " - ");
                const char *msg = col + 1;
                size_t msg_len = (size_t)((rest + rest_len) - msg);
                append_percent_decoded(msg, msg_len, out, cap);
            } else {
                append_percent_decoded(p, p_len, out, cap);
            }
            append_str(out, cap, "\n");
            return;
        }
        if (pre && strcmp(pre, "tel:") == 0) append_str(out, cap, "TEL ");
        else if (pre && strcmp(pre, "mailto:") == 0) append_str(out, cap, "MAIL ");
        else append_str(out, cap, "URL ");
        if (pre) append_str(out, cap, pre);
        for (size_t i = 1; i < payload_len && *cap > 1; ++i) {
            unsigned char c = payload[i];
            if (c >= 32 && c <= 126) { **out = (char)c; (*out)++; (*cap)--; }
            else { append_fmt(out, cap, "%%%02X", c); }
        }
        append_str(out, cap, "\n");
        return;
    }
    if (tnf == 0x01 && type_len == 1 && type[0] == 'T' && payload_len >= 1) {
        uint8_t status = payload[0];
        uint8_t lang_len = status & 0x3F;
        size_t text_off = 1 + lang_len;
        if (text_off > payload_len) text_off = payload_len;
        append_str(out, cap, "Text \"");
        for (size_t i = text_off; i < payload_len && *cap > 1; ++i) {
            unsigned char c = payload[i];
            if (c >= 32 && c <= 126) { **out = (char)c; (*out)++; (*cap)--; }
        }
        append_str(out, cap, "\"\n");
        return;
    }
    if (tnf == 0x01 && type_len == 2 && type[0] == 'S' && type[1] == 'p' && payload_len > 0) {
        const char *url_pre = NULL; const uint8_t *url_bytes = NULL; size_t url_len = 0;
        const uint8_t *title_bytes = NULL; size_t title_len = 0; size_t pos = 0;
        while (pos < payload_len) {
            if (pos + 1 > payload_len) break;
            uint8_t flags = payload[pos++];
            uint8_t tlen = (pos < payload_len) ? payload[pos++] : 0;
            uint32_t plen = 0;
            if (flags & 0x10) { plen = (pos < payload_len) ? payload[pos++] : 0; }
            else { if (pos + 4 > payload_len) break; plen = ((uint32_t)payload[pos] << 24) | ((uint32_t)payload[pos+1] << 16) | ((uint32_t)payload[pos+2] << 8) | payload[pos+3]; pos += 4; }
            if (flags & 0x08) { if (pos < payload_len) pos++; }
            const uint8_t *tt = (pos + tlen <= payload_len) ? &payload[pos] : NULL; pos += tlen;
            const uint8_t *pl = (pos + plen <= payload_len) ? &payload[pos] : NULL; pos += plen;
            if (!tt || !pl) break;
            if (tlen == 1 && tt[0] == 'U' && plen >= 1) { url_pre = (pl[0] < (sizeof(ndef_uri_prefix)/sizeof(ndef_uri_prefix[0])) ? ndef_uri_prefix[pl[0]] : NULL); url_bytes = &pl[1]; url_len = plen - 1; }
            else if (tlen == 1 && tt[0] == 'T' && plen >= 1) { uint8_t ll = pl[0] & 0x3F; size_t toff = 1 + ll; if (toff <= plen) { title_bytes = &pl[toff]; title_len = plen - toff; } }
            if (flags & 0x40) break;
        }
        append_str(out, cap, "SmartPoster ");
        if (url_bytes) {
            append_str(out, cap, "URL "); if (url_pre) append_str(out, cap, url_pre);
            for (size_t i = 0; i < url_len && *cap > 1; ++i) { unsigned char c = url_bytes[i]; if (c >= 32 && c <= 126) { **out = (char)c; (*out)++; (*cap)--; } else { append_fmt(out, cap, "%%%02X", c); } }
        }
        if (title_bytes && title_len) {
            append_str(out, cap, url_bytes ? " | Title \"" : "Title \"");
            for (size_t i = 0; i < title_len && *cap > 1; ++i) { unsigned char c = title_bytes[i]; if (c >= 32 && c <= 126) { **out = (char)c; (*out)++; (*cap)--; } }
            append_str(out, cap, "\"");
        }
        append_str(out, cap, "\n");
        return;
    }
    append_fmt(out, cap, "Record tnf=0x%02X type=", tnf);
    for (uint8_t i = 0; i < type_len; ++i) { if (*cap > 1) { **out = (char)type[i]; (*out)++; (*cap)--; } }
    append_fmt(out, cap, " len=%u\n", (unsigned)payload_len);
}

char* ndef_build_details_from_message(const uint8_t* ndef_msg,
                                      size_t ndef_len,
                                      const uint8_t* uid,
                                      uint8_t uid_len,
                                      const char* card_label) {
    size_t cap = 2048;
    char *out = (char*)malloc(cap);
    if (!out) return NULL;
    char *w = out; *w = '\0';

    if (!card_label) card_label = "NTAG2xx";
    append_fmt(&w, &cap, "Card: %s | UID:", card_label);
    for (uint8_t i = 0; i < uid_len && cap > 3; ++i)
        append_fmt(&w, &cap, " %02X", uid[i]);
    append_str(&w, &cap, "\n");

    if (!ndef_msg || ndef_len == 0) {
        append_str(&w, &cap, "No NDEF message found\n");
        return out;
    }

    size_t rc_count = 0; {
        size_t p = 0, e = ndef_len;
        while (p < e) {
            if (p + 1 > e) break;
            uint8_t flags = ndef_msg[p++];
            uint8_t tlen = (p < e) ? ndef_msg[p++] : 0;
            uint32_t plen = 0;
            if (flags & 0x10) { plen = (p < e) ? ndef_msg[p++] : 0; }
            else { if (p + 4 > e) break; plen = ((uint32_t)ndef_msg[p] << 24) | ((uint32_t)ndef_msg[p+1] << 16) | ((uint32_t)ndef_msg[p+2] << 8) | ndef_msg[p+3]; p += 4; }
            if (flags & 0x08) { if (p < e) p++; }
            p += tlen; p += plen; rc_count++;
            if (flags & 0x40) break;
        }
    }
    append_fmt(&w, &cap, "NDEF: %uB, %u rec\n", (unsigned)ndef_len, (unsigned)rc_count);

    size_t mpos = 0; size_t mend = ndef_len; int rec_idx = 0;
    while (mpos < mend) {
        if (mpos + 1 > mend) break;
        uint8_t flags = ndef_msg[mpos++];
        uint8_t tlen = (mpos < mend) ? ndef_msg[mpos++] : 0;
        uint32_t plen = 0;
        if (flags & 0x10) {
            plen = (mpos < mend) ? ndef_msg[mpos++] : 0;
        } else {
            if (mpos + 4 > mend) break;
            plen = ((uint32_t)ndef_msg[mpos] << 24) | ((uint32_t)ndef_msg[mpos+1] << 16) | ((uint32_t)ndef_msg[mpos+2] << 8) | ndef_msg[mpos+3];
            mpos += 4;
        }
        uint8_t idlen = (flags & 0x08) ? ((mpos < mend) ? ndef_msg[mpos++] : 0) : 0;
        const uint8_t *type = (mpos + tlen <= mend) ? &ndef_msg[mpos] : NULL; mpos += tlen;
        (void)idlen;
        const uint8_t *pl = (mpos + plen <= mend) ? &ndef_msg[mpos] : NULL; mpos += plen;
        if (!type || !pl) break;
        append_fmt(&w, &cap, "R%d: ", ++rec_idx);
        parse_ndef_record(flags & 0x07, type, tlen, pl, plen, &w, &cap);
        if (flags & 0x40) break;
    }

    return out;
}
