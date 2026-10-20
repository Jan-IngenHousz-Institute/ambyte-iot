#include "svc_wire.h"

#include <stdio.h>
#include <string.h>

void svc_wipe(void *data, size_t length)
{
    volatile unsigned char *p = data;
    while (length--) *p++ = 0;
}

uint32_t svc_crc32(uint32_t previous, const void *data, size_t length)
{
    uint32_t crc = previous ^ UINT32_MAX;
    const unsigned char *p = data;
    while (length--) {
        crc ^= *p++;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return crc ^ UINT32_MAX;
}

static int hex_digit(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

bool svc_hex(const char *text, size_t digits, uint64_t *value)
{
    if (digits > 16 || strlen(text) != digits) return false;
    uint64_t n = 0;
    for (size_t i = 0; i < digits; ++i) {
        int d = hex_digit((unsigned char)text[i]);
        if (d < 0) return false;
        n = (n << 4) | (unsigned)d;
    }
    if (value) *value = n;
    return true;
}

bool svc_utf8(const unsigned char *text, size_t length)
{
    for (size_t i = 0; i < length;) {
        unsigned char lead = text[i++];
        unsigned count;
        if (!lead) return false;
        if (lead < 0x80) continue;
        if (lead >= 0xc2 && lead <= 0xdf) count = 1;
        else if (lead >= 0xe0 && lead <= 0xef) count = 2;
        else if (lead >= 0xf0 && lead <= 0xf4) count = 3;
        else return false;
        if (count > length - i) return false;
        if ((lead == 0xe0 && text[i] < 0xa0) ||
            (lead == 0xed && text[i] > 0x9f) ||
            (lead == 0xf0 && text[i] < 0x90) ||
            (lead == 0xf4 && text[i] > 0x8f)) return false;
        while (count--) if (text[i] < 0x80 || text[i++] > 0xbf) return false;
    }
    return true;
}

static const char base64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t svc_base64_encode(const unsigned char *data, size_t length, char *out)
{
    size_t n = 0;
    for (size_t i = 0; i < length; i += 3) {
        unsigned a = data[i];
        unsigned b = i + 1 < length ? data[i + 1] : 0;
        unsigned c = i + 2 < length ? data[i + 2] : 0;
        out[n++] = base64[a >> 2];
        out[n++] = base64[((a & 3) << 4) | (b >> 4)];
        out[n++] = i + 1 < length ? base64[((b & 15) << 2) | (c >> 6)] : '=';
        out[n++] = i + 2 < length ? base64[c & 63] : '=';
    }
    out[n] = 0;
    return n;
}

static int b64_digit(char c)
{
    const char *p = c ? strchr(base64, c) : NULL;
    return p ? (int)(p - base64) : -1;
}

int svc_base64_decode(const char *text, unsigned char *out, size_t capacity)
{
    size_t length = strlen(text), n = 0;
    if (!length || length % 4) return -1;
    for (size_t i = 0; i < length; i += 4) {
        int a = b64_digit(text[i]), b = b64_digit(text[i + 1]);
        int c = text[i + 2] == '=' ? 0 : b64_digit(text[i + 2]);
        int d = text[i + 3] == '=' ? 0 : b64_digit(text[i + 3]);
        bool pad_c = text[i + 2] == '=', pad_d = text[i + 3] == '=';
        if (a < 0 || b < 0 || c < 0 || d < 0 || (pad_c && !pad_d) ||
            ((pad_c || pad_d) && i + 4 != length) ||
            (pad_c && (b & 15)) || (pad_d && !pad_c && (c & 3))) return -1;
        size_t need = pad_c ? 1 : pad_d ? 2 : 3;
        if (need > capacity - n) return -2;
        out[n++] = (unsigned char)((a << 2) | (b >> 4));
        if (!pad_c) out[n++] = (unsigned char)((b << 4) | (c >> 2));
        if (!pad_d) out[n++] = (unsigned char)((c << 6) | d);
    }
    return (int)n;
}

size_t svc_frame(char *out, size_t capacity, uint32_t correlation, const char *json)
{
    size_t length = strlen(json);
    if (length > SVC_JSON_MAX || capacity < length + 20) return 0;
    int prefix = snprintf(out, capacity, "%08lx %s", (unsigned long)correlation, json);
    if (prefix < 0 || (size_t)prefix + 11 > capacity) return 0;
    uint32_t crc = svc_crc32(0, out, (size_t)prefix);
    snprintf(out + prefix, capacity - (size_t)prefix, "*%08lx\n", (unsigned long)crc);
    return (size_t)prefix + 10;
}

void svc_rx_clear(svc_rx_t *rx)
{
    svc_wipe(rx, sizeof *rx);
}

const char *svc_rx_timeout(svc_rx_t *rx, int64_t now_ms)
{
    if (rx->length && now_ms - rx->started_ms >= 2000) {
        svc_rx_clear(rx);
        rx->draining = true;
        return "frame_timeout";
    }
    return NULL;
}

const char *svc_rx_byte(svc_rx_t *rx, unsigned char byte, int64_t now_ms,
                        char **body, uint32_t *correlation)
{
    *body = NULL;
    *correlation = 0;
    if (rx->draining) {
        if (byte == '\n') svc_rx_clear(rx);
        return NULL;
    }
    if (rx->length == 0) rx->started_ms = now_ms;
    if (rx->length == SVC_LINE_MAX ||
        (rx->length == SVC_LINE_MAX - 1 && byte != '\n')) {
        svc_rx_clear(rx);
        rx->draining = byte != '\n';
        return "line_too_long";
    }
    if (byte == 0 || byte == '\r' || (byte != '\n' && (byte < 0x20 || byte > 0x7e))) {
        svc_rx_clear(rx);
        rx->draining = byte != '\n';
        return "bad_frame";
    }
    if (byte != '\n') {
        rx->line[rx->length++] = (char)byte;
        return NULL;
    }
    size_t n = rx->length;
    if (n < 20 || rx->line[n - 9] != '*' || rx->line[8] != ' ') {
        svc_rx_clear(rx);
        return "bad_frame";
    }
    uint64_t crc, id;
    if (!svc_hex(rx->line + n - 8, 8, &crc)) {
        svc_rx_clear(rx);
        return "bad_frame";
    }
    if (svc_crc32(0, rx->line, n - 9) != (uint32_t)crc) {
        svc_rx_clear(rx);
        return "bad_crc";
    }
    rx->line[8] = 0;
    if (!svc_hex(rx->line, 8, &id) || id == 0) {
        svc_rx_clear(rx);
        return "bad_frame";
    }
    rx->line[n - 9] = 0;
    *correlation = (uint32_t)id;
    *body = rx->line + 9;
    return NULL;
}

size_t svc_editor_byte(svc_editor_filter_t *filter, unsigned char byte,
                       unsigned char output[9], bool *dropped)
{
    *dropped = false;
    if (filter->escape) {
        output[0] = byte;
        if (filter->escape == 1 && byte == '[') filter->escape = 2;
        else if (filter->escape == 1 || (byte >= '@' && byte <= '~')) filter->escape = 0;
        return 1;
    }
    if (filter->passthrough) {
        output[0] = byte;
        if (byte == '\n' || byte == '\r') svc_wipe(filter, sizeof *filter);
        return 1;
    }
    if (filter->draining) {
        if (byte == '\n' || byte == '\r') {
            svc_wipe(filter, sizeof *filter);
            *dropped = true;
        }
        return 0;
    }
    if (filter->length < 8 && hex_digit(byte) >= 0) {
        filter->prefix[filter->length++] = byte;
        return 0;
    }
    if (filter->length == 8 && byte == ' ') {
        svc_wipe(filter->prefix, sizeof filter->prefix);
        filter->length = 0;
        filter->draining = true;
        return 0;
    }
    size_t n = filter->length;
    memcpy(output, filter->prefix, n);
    output[n++] = byte;
    svc_wipe(filter, sizeof *filter);
    if (byte == '\033' && n == 1) filter->escape = 1;
    else filter->passthrough = byte != '\r' && byte != '\n';
    return n;
}
