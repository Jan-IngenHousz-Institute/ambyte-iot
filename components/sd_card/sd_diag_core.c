/* Pure-C core of sd_diag (see sd_diag.h). No ESP-IDF dependency: the host
 * test compiles exactly this file. Callers serialize (sd_diag.c holds a
 * spinlock around every mutation). */
#include "sd_diag.h"

#include <stdio.h>
#include <string.h>

_Static_assert(sizeof(sd_diag_block_t) <= 256, "sd_diag block exceeds the 256-B RTC budget");

uint32_t sd_diag_crc(const sd_diag_block_t *b)
{
    const uint8_t *p = (const uint8_t *)b;
    size_t n = offsetof(sd_diag_block_t, crc);
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

bool sd_diag_valid(const sd_diag_block_t *b)
{
    return b->magic == SD_DIAG_MAGIC && b->version == SD_DIAG_VERSION &&
           b->size == sizeof(sd_diag_block_t) && b->crc == sd_diag_crc(b);
}

void sd_diag_seal(sd_diag_block_t *b)
{
    b->crc = sd_diag_crc(b);
}

static void fresh(sd_diag_block_t *b)
{
    memset(b, 0, sizeof *b);
    b->magic = SD_DIAG_MAGIC;
    b->version = SD_DIAG_VERSION;
    b->size = (uint16_t)sizeof *b;
}

void sd_diag_core_boot(sd_diag_block_t *b, bool cpu_reset)
{
    if (cpu_reset && sd_diag_valid(b)) {
        /* Continuity proven by the CRC: keep counting where we left off. */
        b->boot_seq++;
        sd_diag_seal(b);
        return;
    }
    /* Power-on, or RTC contents that did not survive: whatever was counted
     * since the last NVS snapshot is gone. Provisional until the floor merge. */
    fresh(b);
    b->floor_pending = 1;
    b->exact = 0;
    sd_diag_seal(b);
}

static uint16_t sat_add16(uint32_t a, uint32_t b)
{
    uint32_t s = a + b;
    return (uint16_t)(s > 0xFFFFu ? 0xFFFFu : s);
}

static uint32_t sat_add32(uint64_t a, uint64_t b)
{
    uint64_t s = a + b;
    return (uint32_t)(s > 0xFFFFFFFFu ? 0xFFFFFFFFu : s);
}

static void stamp_early(sd_diag_block_t *b);

void sd_diag_core_merge_floor(sd_diag_block_t *b, const sd_diag_block_t *floor)
{
    if (!b->floor_pending) return;
    b->floor_pending = 0;
    if (floor != NULL && sd_diag_valid(floor)) {
        /* Counts recorded before NVS was up (early SD writers) are ADDED to the
         * floor — both are real, disjoint events. */
        for (int w = 0; w < SD_DIAG_W_COUNT; w++)
            for (int o = 0; o < SD_DIAG_OP_COUNT; o++)
                b->cnt[w][o] = sat_add16(b->cnt[w][o], floor->cnt[w][o]);
        for (int r = 0; r < SD_DIAG_REF_COUNT; r++) b->refused[r] = sat_add32(b->refused[r], floor->refused[r]);
        if (b->last.uptime_ms == 0 && b->last.writer == 0 && b->last.op == 0 && b->last.err == 0) b->last = floor->last;
        if (b->ref_first_id == 0) b->ref_first_id = floor->ref_first_id;
        if (b->ref_last_id == 0) { b->ref_last_id = floor->ref_last_id; b->ref_last = floor->ref_last; }
        b->epoch = floor->epoch + 1;
        b->boot_seq = floor->boot_seq + 1;
        /* Continue the generation from the floor (plus the early changes):
         * the glue remembers floor.gen as "already persisted", so a fresh gen
         * restarting at 1 would collide with it and silently skip the next
         * snapshot — even a forced one. Strictly monotonic across power-ons. */
        b->gen += floor->gen;
    } else {
        /* No readable floor (first boot, NVS failure, corrupt snapshot): we
         * cannot even give a floor, and cannot tell those cases apart. */
        b->epoch = 1;
        b->boot_seq = 1;
    }
    b->exact = 0;                   /* a power-on is never proof of continuity */
    b->gen++;
    stamp_early(b);
    sd_diag_seal(b);
}

/* Faults recorded before the floor merge carry boot 0; stamp them with the
 * boot they actually happened in. */
static void stamp_early(sd_diag_block_t *b)
{
    if (b->last.boot_seq == 0 && (b->last.uptime_ms != 0 || b->last.err != 0)) b->last.boot_seq = b->boot_seq;
}

void sd_diag_core_fault(sd_diag_block_t *b, sd_diag_writer_t w, sd_diag_op_t op, int err, uint32_t uptime_ms)
{
    if ((unsigned)w >= SD_DIAG_W_COUNT || (unsigned)op >= SD_DIAG_OP_COUNT) return;
    b->cnt[w][op] = sat_add16(b->cnt[w][op], 1);
    b->last.writer = (uint8_t)w;
    b->last.op = (uint8_t)op;
    b->last.err = (int16_t)(err > 32767 ? 32767 : (err < -32768 ? -32768 : err));
    b->last.uptime_ms = uptime_ms;
    b->last.boot_seq = b->boot_seq;
    b->gen++;
    sd_diag_seal(b);
}

void sd_diag_core_refusal(sd_diag_block_t *b, sd_diag_refusal_t reason, int64_t id, int err,
                          uint8_t blocked, uint8_t sd_state, int64_t wall_ms, uint32_t uptime_ms)
{
    if ((unsigned)reason >= SD_DIAG_REF_COUNT) return;
    b->refused[reason] = sat_add32(b->refused[reason], 1);
    if (id > 0) {
        if (b->ref_first_id == 0) b->ref_first_id = id;
        b->ref_last_id = id;
    }
    b->ref_last.reason = (uint8_t)reason;
    b->ref_last.blocked = blocked;
    b->ref_last.sd_state = sd_state;
    b->ref_last.err = (int16_t)(err > 32767 ? 32767 : (err < -32768 ? -32768 : err));
    b->ref_last.uptime_ms = uptime_ms;
    b->ref_last.wall_ms = wall_ms;
    b->gen++;
    sd_diag_seal(b);
}

bool sd_diag_core_should_persist(const sd_diag_block_t *b, uint32_t persisted_gen, bool persisted_this_boot,
                                 uint32_t last_persist_ms, uint32_t now_ms, bool force)
{
    if (b->floor_pending) return false;           /* never overwrite the floor before merging it */
    if (b->gen == persisted_gen) return false;
    if (force || !persisted_this_boot) return true;
    return (uint32_t)(now_ms - last_persist_ms) >= SD_DIAG_PERSIST_MIN_MS;
}

const char *sd_diag_writer_name(unsigned w)
{
    static const char *const n[SD_DIAG_W_COUNT] = { "evlog", "sdlog", "ambit_ota", "ambit_flash" };
    return w < SD_DIAG_W_COUNT ? n[w] : "?";
}

const char *sd_diag_op_name(unsigned op)
{
    static const char *const n[SD_DIAG_OP_COUNT] = { "open", "write", "flush", "fsync", "close", "truncate",
                                                     "rename", "remove", "mkdir", "stat", "verify", "read" };
    return op < SD_DIAG_OP_COUNT ? n[op] : "?";
}

const char *sd_diag_refusal_name(unsigned r)
{
    static const char *const n[SD_DIAG_REF_COUNT] = { "full", "media", "too_large", "unavailable" };
    return r < SD_DIAG_REF_COUNT ? n[r] : "?";
}

int sd_diag_render_json(const sd_diag_block_t *b, char *buf, size_t cap)
{
    size_t off = 0;
    int n;
#define PUT(...) do {                                               \
        n = snprintf(buf + off, cap - off, __VA_ARGS__);            \
        if (n < 0 || (size_t)n >= cap - off) { return -1; }         \
        off += (size_t)n;                                           \
    } while (0)
    PUT("{\"exact\":%s,\"epoch\":%u,\"boot\":%u,\"faults\":{", b->exact ? "true" : "false",
        (unsigned)b->epoch, (unsigned)b->boot_seq);
    bool first = true;
    for (int w = 0; w < SD_DIAG_W_COUNT; w++) {
        for (int o = 0; o < SD_DIAG_OP_COUNT; o++) {
            if (b->cnt[w][o] == 0) continue;
            PUT("%s\"%s.%s\":%u", first ? "" : ",", sd_diag_writer_name((unsigned)w), sd_diag_op_name((unsigned)o),
                (unsigned)b->cnt[w][o]);
            first = false;
        }
    }
    PUT("}");
    if (b->last.uptime_ms != 0 || b->last.err != 0) {
        PUT(",\"last\":{\"w\":\"%s\",\"op\":\"%s\",\"errno\":%d,\"uptime_ms\":%u,\"boot\":%u}",
            sd_diag_writer_name(b->last.writer), sd_diag_op_name(b->last.op), (int)b->last.err,
            (unsigned)b->last.uptime_ms, (unsigned)b->last.boot_seq);
    }
    PUT(",\"refused\":{\"full\":%u,\"media\":%u,\"too_large\":%u,\"unavailable\":%u,\"first_id\":%lld,\"last_id\":%lld",
        (unsigned)b->refused[0], (unsigned)b->refused[1], (unsigned)b->refused[2], (unsigned)b->refused[3],
        (long long)b->ref_first_id, (long long)b->ref_last_id);
    if (b->refused[0] + b->refused[1] + b->refused[2] + b->refused[3] != 0) {
        PUT(",\"last\":{\"reason\":\"%s\",\"errno\":%d,\"blocked\":%u,\"sd_state\":%u,\"wall_ms\":%lld,\"uptime_ms\":%u}",
            sd_diag_refusal_name(b->ref_last.reason), (int)b->ref_last.err, (unsigned)b->ref_last.blocked,
            (unsigned)b->ref_last.sd_state, (long long)b->ref_last.wall_ms, (unsigned)b->ref_last.uptime_ms);
    }
    PUT("}}");
#undef PUT
    return (int)off;
}
