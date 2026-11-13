/*
 * location_aws_codec.c — pure request/reply handling for AWS IoT Core Device
 * Location (contract in location_aws.h). No RTOS, no I/O: host-tested in
 * tests/test_location_aws.py against the payloads verified on DEV.
 */
#include "location_aws.h"

#include <ctype.h>
#include <stdbool.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

bool location_aws_loc_id(const char *thing_name, char *out, size_t len)
{
    if (thing_name == NULL || out == NULL || len == 0) return false;
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    if (thing_name[0] == '\0') return false;
    for (const unsigned char *p = (const unsigned char *)thing_name; *p; p++) {
        bool keep = (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9');
        size_t need = keep ? 1 : 3;
        /* Never truncate: a too-long id would name another device's topics.
         * Over AWS's 128-char segment limit the client stays inert. */
        if (o + need > LOCATION_AWS_ID_MAX - 1 || o + need + 1 > len) return false;
        if (keep) {
            out[o++] = (char)*p;
        } else {
            /* Injective escape (every byte outside [A-Za-z0-9], '-' included,
             * becomes '-' + two uppercase hex digits): edge_box, edge-box and
             * edge:box get distinct ids and so distinct policy grants. */
            out[o++] = '-';
            out[o++] = hex[*p >> 4];
            out[o++] = hex[*p & 0x0F];
        }
    }
    out[o] = '\0';
    return true;
}

int location_aws_select_aps(const location_aws_ap_t *in, int n, location_aws_ap_t *out, int max_out)
{
    int count = 0;
    for (int i = 0; i < n && in != NULL; i++) {
        /* 0x02: locally administered (randomised, phone hotspot);
         * 0x01: group address. Neither is in a location database. */
        if (in[i].bssid[0] & 0x03) continue;
        static const uint8_t zero[6];
        if (memcmp(in[i].bssid, zero, 6) == 0) continue;
        bool dup = false;
        for (int j = 0; j < count; j++) dup |= memcmp(out[j].bssid, in[i].bssid, 6) == 0;
        if (dup) continue;
        /* Insertion by RSSI, strongest first, bounded. */
        int pos = count < max_out ? count : max_out;
        while (pos > 0 && out[pos - 1].rssi < in[i].rssi) {
            if (pos < max_out) out[pos] = out[pos - 1];
            pos--;
        }
        if (pos < max_out) {
            out[pos] = in[i];
            if (count < max_out) count++;
        }
    }
    return count;
}

static size_t finish(int n, size_t cap)
{
    return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

size_t location_aws_wifi_request(const char *message_id, int64_t now_utc,
                                 const location_aws_ap_t *aps, int n, char *out, size_t cap)
{
    if (message_id == NULL || aps == NULL || n < 2 || out == NULL) return 0;
    /* Strict schema: Timestamp must be a STRING (a number is a 400); unknown
     * keys are a 400. */
    int w = snprintf(out, cap, "{\"Timestamp\":\"%lld\",\"MessageId\":\"%s\",\"WiFiAccessPoints\":[",
                     (long long)now_utc, message_id);
    if (w <= 0 || (size_t)w >= cap) return 0;
    size_t used = (size_t)w;
    for (int i = 0; i < n; i++) {
        const uint8_t *b = aps[i].bssid;
        w = snprintf(out + used, cap - used,
                     "%s{\"MacAddress\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"Rss\":%d}",
                     i ? "," : "", b[0], b[1], b[2], b[3], b[4], b[5], aps[i].rssi);
        if (w <= 0 || (size_t)w >= cap - used) return 0;
        used += (size_t)w;
    }
    w = snprintf(out + used, cap - used, "]}");
    return finish(w, cap - used) ? used + (size_t)w : 0;
}

size_t location_aws_ip_request(const char *message_id, int64_t now_utc, const char *ip,
                               char *out, size_t cap)
{
    if (message_id == NULL || ip == NULL || ip[0] == '\0' || out == NULL) return 0;
    int w = snprintf(out, cap, "{\"Timestamp\":\"%lld\",\"MessageId\":\"%s\",\"Ip\":{\"IpAddress\":\"%s\"}}",
                     (long long)now_utc, message_id, ip);
    return finish(w, cap);
}

static bool parse_ipv4(const char *s, uint8_t a[4])
{
    int part = 0;
    const char *p = s;
    while (part < 4) {
        if (!isdigit((unsigned char)*p)) return false;
        int v = 0, digits = 0;
        while (isdigit((unsigned char)*p)) {
            if (++digits > 3) return false;
            v = v * 10 + (*p++ - '0');
        }
        if (v > 255 || (digits > 1 && p[-digits] == '0')) return false;
        a[part++] = (uint8_t)v;
        if (part < 4 && *p++ != '.') return false;
    }
    return *p == '\0';
}

static bool ipv4_public(const uint8_t a[4])
{
    if (a[0] == 0 || a[0] == 10 || a[0] == 127 || a[0] >= 224) return false;
    if (a[0] == 100 && (a[1] & 0xC0) == 64) return false;   /* 100.64/10 shared (CGNAT) */
    if (a[0] == 169 && a[1] == 254) return false;
    if (a[0] == 172 && (a[1] & 0xF0) == 16) return false;
    if (a[0] == 192 && a[1] == 168) return false;
    if (a[0] == 192 && a[1] == 0 && (a[2] == 0 || a[2] == 2)) return false;
    if (a[0] == 198 && (a[1] == 18 || a[1] == 19)) return false;
    if (a[0] == 198 && a[1] == 51 && a[2] == 100) return false;
    if (a[0] == 203 && a[1] == 0 && a[2] == 113) return false;
    return true;
}

/* Conservative IPv6 literal check: hex groups and colons, one "::" at most,
 * global unicast (2000::/3) only. */
static bool ipv6_public(const char *s)
{
    size_t n = strlen(s);
    if (n < 3 || n > 39) return false;
    int groups = 0, run = 0, doubles = 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == ':') {
            if (i + 1 < n && s[i + 1] == ':') { doubles++; i++; }
            if (run == 0 && i > 1 && doubles == 0) return false;
            run = 0;
            continue;
        }
        if (!isxdigit((unsigned char)c) || ++run > 4) return false;
        if (run == 1) groups++;
    }
    if (doubles > 1 || groups > 8 || (doubles == 0 && groups != 8)) return false;
    unsigned first = (unsigned)strtoul(s, NULL, 16);
    if (s[0] == ':' || (first & 0xE000) != 0x2000) return false;
    /* 2001:db8::/32 is documentation (RFC 3849), nobody's public address. */
    const char *colon = strchr(s, ':');
    if (first == 0x2001 && colon && strtoul(colon + 1, NULL, 16) == 0xdb8 &&
        (colon[1] != ':')) {
        return false;
    }
    return true;
}

bool location_aws_public_ip(const char *text, char *out, size_t len)
{
    if (text == NULL || out == NULL || len == 0) return false;
    char buf[48];
    size_t n = strlen(text);
    if (n > 0 && text[n - 1] == '\n') n--;
    if (n > 0 && text[n - 1] == '\r') n--;
    if (n == 0 || n >= sizeof buf || n + 1 > len) return false;
    memcpy(buf, text, n);
    buf[n] = '\0';
    uint8_t a[4];
    bool ok = strchr(buf, ':') ? ipv6_public(buf) : (parse_ipv4(buf, a) && ipv4_public(a));
    if (ok) memcpy(out, buf, n + 1);
    return ok;
}

bool location_aws_parse_reply(bool rejected_topic, const char *payload, size_t len,
                              location_aws_reply_t *out)
{
    if (payload == NULL || out == NULL) return false;
    memset(out, 0, sizeof *out);
    cJSON *root = cJSON_ParseWithLength(payload, len);
    if (root == NULL) return false;
    bool ok = false;
    const cJSON *props = cJSON_GetObjectItemCaseSensitive(root, "properties");
    const cJSON *mid = cJSON_GetObjectItemCaseSensitive(rejected_topic ? root : props, "messageId");
    if (cJSON_IsString(mid)) snprintf(out->message_id, sizeof out->message_id, "%s", mid->valuestring);
    if (rejected_topic) {
        const cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "errorCode");
        out->accepted = false;
        out->error_code = cJSON_IsNumber(code) ? (int)code->valuedouble : 0;
        ok = true;
    } else {
        const cJSON *coords = cJSON_GetObjectItemCaseSensitive(root, "coordinates");
        const cJSON *acc = cJSON_IsObject(props)
                           ? cJSON_GetObjectItemCaseSensitive(props, "horizontalAccuracy") : NULL;
        if (cJSON_IsArray(coords) && cJSON_GetArraySize(coords) >= 2) {
            const cJSON *lon = cJSON_GetArrayItem(coords, 0);   /* GeoJSON: [lon, lat] */
            const cJSON *lat = cJSON_GetArrayItem(coords, 1);
            if (cJSON_IsNumber(lon) && cJSON_IsNumber(lat) && isfinite(lat->valuedouble) &&
                isfinite(lon->valuedouble) && fabs(lat->valuedouble) <= 90.0 &&
                fabs(lon->valuedouble) <= 180.0) {
                out->accepted = true;
                out->lat = lat->valuedouble;
                out->lon = lon->valuedouble;
                /* Missing accuracy stays NAN: the acceptance gate rejects it. */
                out->accuracy_m = cJSON_IsNumber(acc) ? acc->valuedouble : NAN;
                ok = true;
            }
        }
    }
    cJSON_Delete(root);
    return ok;
}
