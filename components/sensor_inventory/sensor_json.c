#include "sensor_inventory.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct { sensor_json_sink_t sink; void *context; size_t count; bool ok; } writer_t;
static void bytes(writer_t *w, const void *s, size_t size)
{
    if (!w->ok) return;
    if (size > SENSOR_SNAPSHOT_MAX - w->count || !w->sink(w->context, s, size)) { w->ok = false; return; }
    w->count += size;
}
static void raw(writer_t *w, const char *s) { bytes(w, s, strlen(s)); }
static void number(writer_t *w, const char *fmt, ...)
{
    char b[512]; va_list a; va_start(a, fmt); int n = vsnprintf(b, sizeof b, fmt, a); va_end(a);
    if (n < 0 || (size_t)n >= sizeof b) { w->ok = false; return; }
    bytes(w, b, (size_t)n);
}
static bool utf8(const unsigned char *s, size_t n)
{
    for (size_t i = 0; i < n;) {
        unsigned char c = s[i++]; unsigned more;
        if (c < 128) continue;
        if (c >= 0xc2 && c <= 0xdf) more = 1;
        else if (c >= 0xe0 && c <= 0xef) more = 2;
        else if (c >= 0xf0 && c <= 0xf4) more = 3;
        else return false;
        if (more > n - i || (c == 0xe0 && s[i] < 0xa0) || (c == 0xed && s[i] > 0x9f) ||
            (c == 0xf0 && s[i] < 0x90) || (c == 0xf4 && s[i] > 0x8f)) return false;
        while (more--) if ((s[i++] & 0xc0) != 0x80) return false;
    }
    return true;
}
static void string(writer_t *w, const char *s, size_t capacity)
{
    size_t n = 0; while (n < capacity && s[n]) ++n;
    if (!n || !utf8((const unsigned char *)s, n)) { raw(w, "null"); return; }
    raw(w, "\"");
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') { raw(w, "\\"); bytes(w, &c, 1); }
        else if (c < 32) number(w, "\\u%04x", c);
        else bytes(w, &c, 1);
    }
    raw(w, "\"");
}
static void base64(writer_t *w, const unsigned char *b, size_t n)
{
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    raw(w, "\"");
    for (size_t i = 0; i < n; i += 3) {
        unsigned x = (unsigned)b[i] << 16;
        if (i + 1 < n) x |= (unsigned)b[i+1] << 8;
        if (i + 2 < n) x |= b[i+2];
        char out[4] = {alphabet[x >> 18], alphabet[(x >> 12) & 63],
            i+1 < n ? alphabet[(x >> 6) & 63] : '=', i+2 < n ? alphabet[x & 63] : '='};
        bytes(w, out, sizeof out);
    }
    raw(w, "\"");
}
static uint32_t u32(const unsigned char *p)
{ return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void floating(writer_t *w, const unsigned char *p)
{
    uint32_t bits = u32(p); float value; memcpy(&value, &bits, 4);
    if (isfinite(value)) number(w, "%.9g", (double)value); else raw(w, "null");
}
static void observation_end(writer_t *w, const sensor_stamp_t *s, const char *source, bool available)
{
    raw(w, ",\"source\":");
    if (available) string(w, source, strlen(source)); else raw(w, "null");
    raw(w, ",\"observed_uptime_ms\":");
    if (available && s->valid) number(w, "%lld", (long long)s->uptime_ms); else raw(w, "null");
    raw(w, ",\"observed_utc_ms\":");
    if (available && s->valid && s->utc_ms) number(w, "%lld", (long long)s->utc_ms); else raw(w, "null");
    raw(w, ",\"clock\":");
    if (available && s->valid && s->clock && s->clock <= 3) {
        static const char *const sources[] = {"", "rtc", "hwm", "sntp"};
        raw(w, "{\"source\":"); string(w, sources[s->clock], 4);
        raw(w, s->suspect ? ",\"suspect\":true}" : ",\"suspect\":false}");
    } else raw(w, "null");
    raw(w, "}");
}
static void calibration(writer_t *w, const unsigned char *b)
{
    raw(w, "{\"raw_base64\":"); base64(w, b, SENSOR_CAL_BYTES);
    raw(w, ",\"ambit_name\":"); string(w, (const char *)b, 20);
    raw(w, ",\"mlx_coef\":[");
    for (unsigned i = 0; i < 14; ++i) {
        uint32_t bits = u32(b + 20 + 4*i); int32_t value; memcpy(&value, &bits, 4);
        if (i) raw(w, ",");
        number(w, "%ld", (long)value);
    }
    raw(w, "],\"adpd\":[");
    for (unsigned i = 0; i < 6; ++i) { if (i) raw(w, ","); number(w, "%lu", (unsigned long)u32(b + 76 + 4*i)); }
    raw(w, "]");
    static const char *const names[] = {"temp_offset", "temp_slope", "actinic_coef", "spec_coef"};
    for (unsigned i = 0; i < 4; ++i) { number(w, ",\"%s\":", names[i]); floating(w, b + 100 + 4*i); }
    for (unsigned i = 0; i < 5; ++i) number(w, ",\"act_%u\":%u", 50*(i+1), (unsigned)b[116+2*i] | (unsigned)b[117+2*i] << 8);
    raw(w, ",\"mlx_emissivity\":"); floating(w, b + 128);
    raw(w, ",\"sun_coef\":"); floating(w, b + 132);
    raw(w, ",\"tick_factor\":"); floating(w, b + 136); raw(w, "}");
}
static void spectral(writer_t *w, const unsigned char *b)
{
    raw(w, "{\"format\":1");
    static const char *const names[] = {"spec_offset", "spec_sens", "par_weight"};
    for (unsigned k = 0; k < 3; ++k) {
        number(w, ",\"%s\":[", names[k]);
        for (unsigned i = 0; i < 10; ++i) { if (i) raw(w, ","); floating(w, b + 4 + 40*k + 4*i); }
        raw(w, "]");
    }
    raw(w, ",\"par_slope\":"); floating(w, b + 124);
    raw(w, ",\"par_intercept\":"); floating(w, b + 128); raw(w, "}");
}
bool sensor_snapshot_json(const sensor_snapshot_t *s, sensor_json_sink_t sink, void *context)
{
    if (!s || !sink || !s->boot_id) return false;
    writer_t w = {.sink = sink, .context = context, .ok = true};
    number(&w, "{\"contract\":1,\"boot_id\":\"%016llx\",\"captured_uptime_ms\":%lld,\"refresh\":{\"wire_transactions\":[",
        (unsigned long long)s->boot_id, (long long)s->captured_uptime_ms);
    for (unsigned i = 0; i < SENSOR_PORTS; ++i) { if (i) raw(&w, ","); number(&w, "%lu", (unsigned long)s->refresh.wire_transactions[i]); }
    number(&w, "],\"deferred_acquisition\":%lu,\"run_start_waits\":%lu,\"run_start_wait_max_us\":%lu,\"stdout_hold_max_us\":%lu,\"pin_heap_bytes\":%lu,\"pin_heap_peak_bytes\":%lu,\"job_start_waits\":%lu,\"job_start_wait_max_us\":%lu},\"ports\":[",
        (unsigned long)s->refresh.deferred_acquisition, (unsigned long)s->refresh.run_start_waits,
        (unsigned long)s->refresh.run_start_wait_max_us, (unsigned long)s->refresh.stdout_hold_max_us,
        (unsigned long)s->refresh.pin_heap_bytes, (unsigned long)s->refresh.pin_heap_peak_bytes,
        (unsigned long)s->refresh.job_start_waits, (unsigned long)s->refresh.job_start_wait_max_us);
    for (unsigned ch = 0; ch < SENSOR_PORTS; ++ch) {
        const sensor_port_snapshot_t *p = &s->ports[ch]; const ambit_device_info_t *e = &p->identity;
        int64_t age = p->presence_stamp.valid ? s->captured_uptime_ms - p->presence_stamp.uptime_ms : -1;
        if (age < 0 && p->presence_stamp.valid) age = 0;
        const char *reason = !p->seen ? "never_observed" : p->revalidation_failed ? "revalidation_failed" : age > SENSOR_REFRESH_MS ? "age" : NULL;
        const char *state = (s->stalled & (1u << ch)) ? "stalled" : (s->busy & (1u << ch)) ? "busy" : p->seen && !p->present ? "absent" : e->valid ? "known" : "unknown";
        if (ch) raw(&w, ",");
        number(&w, "{\"port\":%u,\"state\":\"%s\",\"generation\":%lu,\"stale\":%s,\"stale_reason\":",
            ch, state, (unsigned long)p->generation, reason ? "true" : "false");
        if (reason) string(&w, reason, strlen(reason)); else raw(&w, "null");
        raw(&w, ",\"age_ms\":"); if (age >= 0) number(&w, "%lld", (long long)age); else raw(&w, "null");
        raw(&w, ",\"presence\":{\"value\":"); raw(&w, !p->seen ? "null" : p->present ? "true" : "false");
        observation_end(&w, &p->presence_stamp, "last_read", p->seen);
        raw(&w, ",\"sensor_id\":{\"value\":"); if (e->valid) string(&w, e->device_id, sizeof e->device_id); else raw(&w, "null");
        observation_end(&w, &p->fw_stamp, "identity_cache", e->valid);
        raw(&w, ",\"firmware\":{\"value\":"); if (e->valid) string(&w, e->fw_version, sizeof e->fw_version); else raw(&w, "null");
        observation_end(&w, &p->fw_stamp, "identity_cache", e->valid);
        raw(&w, ",\"hw_rev\":{\"value\":"); if (e->valid && e->hw_rev) number(&w, "%u", e->hw_rev); else raw(&w, "null");
        observation_end(&w, &p->fw_stamp, "identity_cache", e->valid && e->hw_rev);
        raw(&w, ",\"calibration_digest\":{\"value\":");
        if (p->have_cal) number(&w, "{\"algorithm\":\"crc32-ieee\",\"scope\":\"calibration_140\",\"hex\":\"%08lx\"}", (unsigned long)e->cal_version);
        else raw(&w, "null");
        observation_end(&w, &p->cal_stamp, "identity_cache", p->have_cal);
        raw(&w, ",\"calibration\":{\"value\":"); if (p->have_cal) calibration(&w, p->calibration); else raw(&w, "null");
        observation_end(&w, &p->cal_stamp, "last_read", p->have_cal);
        raw(&w, ",\"spectral\":{\"value\":"); if (p->have_spec) spectral(&w, p->spectral); else raw(&w, "null");
        observation_end(&w, &p->spec_stamp, "last_read", p->have_spec);
        raw(&w, "}");
    }
    raw(&w, "],\"acquisition_recovery\":{\"wire_transactions\":[");
    for (unsigned ch = 0; ch < SENSOR_PORTS; ++ch) {
        if (ch) raw(&w, ",");
        number(&w, "%lu", (unsigned long)s->recovery_wire_transactions[ch]);
    }
    raw(&w, "],\"discarded_bytes\":[");
    for (unsigned ch = 0; ch < SENSOR_PORTS; ++ch) {
        if (ch) raw(&w, ",");
        number(&w, "%lu", (unsigned long)s->recovery_discarded_bytes[ch]);
    }
    raw(&w, "],\"boot_idle\":[");
    for (unsigned ch = 0; ch < SENSOR_PORTS; ++ch) {
        if (ch) raw(&w, ",");
        number(&w, "%lu", (unsigned long)s->recovery_boot_idle[ch]);
    }
    raw(&w, "]}}"); return w.ok;
}
typedef struct { unsigned char *out; size_t offset, capacity, count, used; } slice_t;
static bool slice(void *context, const void *data, size_t size)
{
    slice_t *s = context;
    size_t skip = s->count < s->offset ? s->offset - s->count : 0;
    if (skip < size && s->used < s->capacity) {
        size_t n = size - skip; if (n > s->capacity - s->used) n = s->capacity - s->used;
        memcpy(s->out + s->used, (const unsigned char *)data + skip, n); s->used += n;
    }
    s->count += size; return true;
}
size_t sensor_snapshot_slice(const sensor_snapshot_t *s, size_t offset, void *out, size_t capacity, size_t *total)
{
    slice_t b = {.out = out, .offset = offset, .capacity = capacity};
    bool ok = sensor_snapshot_json(s, slice, &b);
    if (total) *total = ok ? b.count : 0;
    return ok ? b.used : 0;
}
