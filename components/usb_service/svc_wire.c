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
        /* Timeout is an explicit fresh-frame boundary. Keeping drain set
         * would silently eat the client's next complete reconnect probe.
         * No old byte survives; a late suffix must pass the full correlation,
         * length and CRC checks as a standalone line or gets its own error. */
        svc_rx_clear(rx);
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
    if (n < 18 || rx->line[n - 9] != '*' || rx->line[8] != ' ') {
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

/* A valid frame can start after ANY editor bytes. Once an eight-hex/space
 * candidate occurs inside ordinary input, retain a bounded suffix until LF
 * rather than assuming it is either a frame or a console argument. This also
 * preserves legacy SHA/release tokens and quoted values containing svc.*.
 * Rolling the window keeps nested candidates protected after a stray prefix. */
static size_t editor_framed_suffix(const unsigned char *bytes, size_t n)
{
    if (n < 19 || bytes[n - 1] != '\n' || bytes[n - 10] != '*') return n;
    uint64_t expected = 0;
    char crc[9]; memcpy(crc, bytes + n - 9, 8); crc[8] = 0;
    bool canonical_crc = svc_hex(crc, 8, &expected);
    for (size_t start = 0; start + 19 <= n; ++start) {
        if (n - start > SVC_LINE_MAX || bytes[start + 8] != ' ') continue;
        bool hex = true;
        for (size_t i = 0; i < 8; ++i) if (hex_digit(bytes[start + i]) < 0) hex = false;
        if (!hex) continue;
        /* Credential-bearing frames with a damaged CRC also stay out of the
         * editor. A quoted literal's trailing quote is not a frame trailer. */
        static const char join[] = "svc.wifi.join ";
        bool credential = n - start >= 19 + sizeof join - 1 &&
                          !memcmp(bytes + start + 9, join, sizeof join - 1);
        if (credential || (canonical_crc && svc_crc32(0, bytes + start, n - start - 10) == expected))
            return start;
    }
    return n;
}
static size_t editor_credential_header(const unsigned char *bytes, size_t n)
{
    /* Reserve the private join header through its operation and SSID tokens,
     * BEFORE the first password byte. Do not wait for a valid trailer: cancel,
     * corruption and oversized requests must not release a password to echo.
     * Requiring those tokens avoids mistaking a legacy svc.* release token or
     * a SHA argument for a join. Literal full join headers are reserved input. */
    static const char action[] = "svc.wifi.join ";
    const size_t op_at = 9 + sizeof action - 1, ssid_at = op_at + 17;
    if (!n || bytes[n - 1] != ' ') return n;
    for (size_t start = 0; start + ssid_at + 2 <= n; ++start) {
        if (bytes[start + 8] != ' ' || memcmp(bytes + start + 9, action, sizeof action - 1) ||
            bytes[start + op_at + 16] != ' ') continue;
        bool valid = true;
        for (size_t i = 0; i < 8; ++i) if (hex_digit(bytes[start + i]) < 0) valid = false;
        for (size_t i = 0; i < 16; ++i) if (hex_digit(bytes[start + op_at + i]) < 0) valid = false;
        for (size_t i = start + ssid_at; i + 1 < n; ++i)
            if (bytes[i] <= ' ' || bytes[i] > 0x7e) valid = false;
        if (valid) return start;
    }
    return n;
}
static void editor_emitted(svc_editor_filter_t *filter, const unsigned char *bytes, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = bytes[i];
        if (c == '\n' || c == '\r' || c == 3 || c == 21) filter->passthrough = false;
        else filter->passthrough = true;
    }
}
size_t svc_editor_byte(svc_editor_filter_t *filter, unsigned char byte,
                       unsigned char output[SVC_EDITOR_OUTPUT_MAX], bool *dropped)
{
    *dropped = false;
    if (filter->draining) {
        if (byte == '\n' || byte == '\r') {
            svc_wipe(filter, sizeof *filter);
            *dropped = true;
        }
        return 0;
    }
    if (filter->candidate) {
        filter->prefix[filter->length++] = byte;
        size_t secret = editor_credential_header(filter->prefix, filter->length);
        if (secret != filter->length) {
            memcpy(output, filter->prefix, secret);
            svc_wipe(filter, sizeof *filter); filter->draining = true;
            return secret;
        }
        if (byte == '\n' || byte < 0x20 || byte > 0x7e) {
            size_t released = byte == '\n' ? editor_framed_suffix(filter->prefix, filter->length) : filter->length;
            *dropped = released != filter->length;
            memcpy(output, filter->prefix, released);
            svc_wipe(filter, sizeof *filter);
            editor_emitted(filter, output, released);
            return released;
        }
        if (filter->length == sizeof filter->prefix) {
            output[0] = filter->prefix[0];
            memmove(filter->prefix, filter->prefix + 1, --filter->length);
            filter->prefix[filter->length] = 0;
            editor_emitted(filter, output, 1);
            return 1;
        }
        return 0;
    }
    if (filter->length < 8 && hex_digit(byte) >= 0) {
        filter->prefix[filter->length++] = byte;
        return 0;
    }
    if (filter->length == 8 && hex_digit(byte) >= 0) {
        output[0] = filter->prefix[0];
        memmove(filter->prefix, filter->prefix + 1, 7); filter->prefix[7] = byte;
        editor_emitted(filter, output, 1);
        return 1;
    }
    if (filter->length == 8 && byte == ' ') {
        if (!filter->passthrough) {
            svc_wipe(filter, sizeof *filter); filter->draining = true;
        } else {
            filter->prefix[filter->length++] = byte; filter->candidate = true;
        }
        return 0;
    }
    size_t released = filter->length;
    memcpy(output, filter->prefix, released); output[released++] = byte;
    bool prior = filter->passthrough;
    svc_wipe(filter, sizeof *filter); filter->passthrough = prior;
    editor_emitted(filter, output, released);
    return released;
}
