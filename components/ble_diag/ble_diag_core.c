#include "ble_diag_core.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

bool ble_diag_snap_stage(ble_diag_snap_t *s, const char *json, size_t len)
{
    if (s == NULL || json == NULL || len == 0 || len > s->cap) return false;
    uint8_t *staging = s->buf[s->active ^ 1u];
    if (staging == NULL) return false;
    memcpy(staging, json, len);
    return true;
}

void ble_diag_snap_publish(ble_diag_snap_t *s, size_t len, int64_t now_ms)
{
    s->active ^= 1u;
    s->len = (uint32_t)len;
    s->built_ms = now_ms;
    /* 0 means "nothing built yet" to the client: never hand it out again on
     * wrap (a 15-min heartbeat wraps in ~122,000 years, but the host test
     * pins the rule rather than the arithmetic). */
    if (++s->seq == 0u) s->seq = 1u;
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

size_t ble_diag_snap_page(const ble_diag_snap_t *s, uint32_t offset,
                          uint8_t out[BLE_DIAG_PAGE_MAX])
{
    put_le32(out, s->seq);
    if (s->seq == 0u || offset >= s->len) return 4u;
    uint32_t n = s->len - offset;
    if (n > BLE_DIAG_PAGE_DATA_MAX) n = BLE_DIAG_PAGE_DATA_MAX;
    memcpy(out + 4, s->buf[s->active] + offset, n);
    return 4u + n;
}

bool ble_diag_parse_offset(const uint8_t *data, size_t len, uint32_t *offset)
{
    if (data == NULL || offset == NULL || len != 4u) return false;
    *offset = (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
              ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
    return true;
}

static int finish(int n, size_t cap, size_t ceiling)
{
    if (n < 0 || (size_t)n >= cap || (size_t)n > ceiling) return -1;
    return n;
}

/* Bounded, so every field has a fixed worst-case width (the host test pins
 * the worst case under both ceilings). */
static uint32_t clamp_age(int64_t age_ms)
{
    if (age_ms < 0) return 0u;
    return age_ms > (int64_t)UINT32_MAX ? UINT32_MAX : (uint32_t)age_ms;
}

int ble_diag_render_meta(char *out, size_t cap, uint32_t seq, uint32_t len,
                         int64_t age_ms, const char *fw, const char *mac)
{
    if (out == NULL || cap == 0) return -1;
    char fw_safe[BLE_DIAG_FW_MAX + 1];
    size_t k = 0;
    for (const char *p = fw ? fw : ""; *p && k < BLE_DIAG_FW_MAX; p++) {
        const char c = *p;
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                        (c >= 'A' && c <= 'Z') || c == '.' || c == '_' ||
                        c == '+' || c == '-';
        if (ok) fw_safe[k++] = c;
    }
    fw_safe[k] = '\0';
    char age[16];
    if (age_ms < 0) snprintf(age, sizeof age, "null");
    else snprintf(age, sizeof age, "%" PRIu32, clamp_age(age_ms));
    char mac_safe[18];
    snprintf(mac_safe, sizeof mac_safe, "%s", mac ? mac : "");
    for (char *p = mac_safe; *p; p++) {
        if (*p == '"' || *p == '\\' || (unsigned char)*p < 0x20) *p = '?';
    }
    const int n = snprintf(out, cap,
                           "{\"v\":0,\"seq\":%" PRIu32 ",\"len\":%" PRIu32
                           ",\"age_ms\":%s,\"fw\":\"%s\",\"mac\":\"%s\"}",
                           seq, len, age, fw_safe, mac_safe);
    return finish(n, cap, BLE_DIAG_META_MAX);
}

int ble_diag_render_live(char *out, size_t cap, const ble_diag_live_t *l)
{
    if (out == NULL || cap == 0 || l == NULL) return -1;
    static const ble_diag_live_t none = {.reason = "unavailable"};
    const ble_diag_live_t *v = l->have ? l : &none;
    char age[16];
    if (!l->have) snprintf(age, sizeof age, "null");
    else snprintf(age, sizeof age, "%" PRIu32, clamp_age(l->age_ms));
    /* reason comes from power_read_reason_name(): a fixed [a-z_] vocabulary.
     * Anything else is replaced, never escaped, to keep the width bounded. */
    const char *reason = v->reason ? v->reason : "unavailable";
    for (const char *p = reason; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || *p == '_') || (p - reason) >= 24) {
            reason = "unknown";
            break;
        }
    }
    const int n = snprintf(out, cap,
                           "{\"age_ms\":%s,\"vin_mv\":%u,\"vsys_mv\":%u,\"vbat_mv\":%u,"
                           "\"iin_ma\":%u,\"ichg_ma\":%u,\"status\":%u,\"fault\":%u,"
                           "\"valid\":%s,\"reason\":\"%s\"}",
                           age, (unsigned)v->vin_mv, (unsigned)v->vsys_mv,
                           (unsigned)v->vbat_mv, (unsigned)v->iin_ma,
                           (unsigned)v->ichg_ma, (unsigned)v->status,
                           (unsigned)v->fault,
                           (l->have && l->valid) ? "true" : "false", reason);
    return finish(n, cap, BLE_DIAG_LIVE_MAX);
}

void ble_diag_format_name(const uint8_t mac[6], char out[12])
{
    snprintf(out, 12, "Ambyte-%02X%02X", mac[4], mac[5]);
}

void ble_diag_format_mac(const uint8_t mac[6], char out[18])
{
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}
