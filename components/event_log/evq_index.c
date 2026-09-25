/*
 * evq_index.c — segment index for the two-media event queue. See evq_index.h
 * for the line format and docs/evq-sd-overflow.md for the state machine.
 */
#include "evq_index.h"
#include "evq_fault.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ── CRC32 (IEEE, reflected) ─────────────────────────────────────────────
 * Table-driven and self-contained so host and target compute bit-identical
 * values without pulling esp_rom into the host harness. 1 KiB of .rodata. */
static uint32_t s_crc_table[256];
static bool     s_crc_ready = false;

static void evq_crc_init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1U) ? (0xEDB88320U ^ (c >> 1)) : (c >> 1);
        s_crc_table[i] = c;
    }
    s_crc_ready = true;
}

uint32_t evq_crc32(uint32_t crc, const void *buf, size_t len)
{
    if (!s_crc_ready) evq_crc_init();
    const uint8_t *p = (const uint8_t *)buf;
    crc = ~crc;
    while (len--) crc = s_crc_table[(crc ^ *p++) & 0xFFU] ^ (crc >> 8);
    return ~crc;
}

const char *evq_seg_state_name(uint8_t state)
{
    switch (state) {
    case EVQ_SEG_FLASH:     return "FLASH";
    case EVQ_SEG_SPOOLED:   return "SPOOLED";
    case EVQ_SEG_SD_ONLY:   return "SD_ONLY";
    case EVQ_SEG_DELIVERED: return "DELIVERED";
    case EVQ_SEG_REIMPORT:  return "REIMPORT";
    default:                return "?";
    }
}

/* ── table ──────────────────────────────────────────────────────────────── */

esp_err_t evq_index_init(evq_index_t *ix, size_t cap)
{
    memset(ix, 0, sizeof *ix);
    /* >1 KiB → PSRAM on the normal board (CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL). */
    ix->segs = calloc(cap, sizeof *ix->segs);
    if (ix->segs == NULL) return ESP_ERR_NO_MEM;
    ix->cap = cap;
    return ESP_OK;
}

void evq_index_free(evq_index_t *ix)
{
    free(ix->segs);
    memset(ix, 0, sizeof *ix);
}

static size_t evq_lower_bound(const evq_index_t *ix, uint32_t seq)
{
    size_t lo = 0, hi = ix->n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (ix->segs[mid].seq < seq) lo = mid + 1; else hi = mid;
    }
    return lo;
}

evq_seg_t *evq_index_find(evq_index_t *ix, uint32_t seq)
{
    size_t i = evq_lower_bound(ix, seq);
    return (i < ix->n && ix->segs[i].seq == seq) ? &ix->segs[i] : NULL;
}

evq_seg_t *evq_index_add(evq_index_t *ix, uint32_t seq)
{
    size_t i = evq_lower_bound(ix, seq);
    if (i < ix->n && ix->segs[i].seq == seq) return &ix->segs[i];
    if (ix->n >= ix->cap) return NULL;
    memmove(&ix->segs[i + 1], &ix->segs[i], (ix->n - i) * sizeof *ix->segs);
    memset(&ix->segs[i], 0, sizeof *ix->segs);
    ix->segs[i].seq = seq;
    ix->segs[i].reimport_remaining = -1;
    ix->n++;
    return &ix->segs[i];
}

void evq_index_remove(evq_index_t *ix, uint32_t seq)
{
    size_t i = evq_lower_bound(ix, seq);
    if (i >= ix->n || ix->segs[i].seq != seq) return;
    memmove(&ix->segs[i], &ix->segs[i + 1], (ix->n - i - 1) * sizeof *ix->segs);
    ix->n--;
}

/* ── line codec ─────────────────────────────────────────────────────────── */

int evq_index_format_line(char *dst, size_t cap, const char *body)
{
    uint32_t crc = evq_crc32(0, body, strlen(body));
    int n = snprintf(dst, cap, "%s *%08" PRIx32 "\n", body, crc);
    return (n < 0 || (size_t)n >= cap) ? -1 : n;
}

bool evq_index_parse_line(const char *line, char *body, size_t body_cap)
{
    size_t len = strlen(line);
    if (len < 12 || line[len - 1] != '\n') return false;          /* torn */
    const char *star = NULL;
    for (size_t i = len; i-- > 0;) { if (line[i] == '*') { star = line + i; break; } }
    if (star == NULL || star == line || star[-1] != ' ') return false;
    size_t blen = (size_t)(star - 1 - line);
    if (blen == 0 || blen >= body_cap) return false;
    char hex[9];
    size_t hl = (size_t)(line + len - 1 - (star + 1));
    if (hl != 8) return false;
    memcpy(hex, star + 1, 8); hex[8] = '\0';
    char *end = NULL;
    unsigned long want = strtoul(hex, &end, 16);
    if (end == NULL || *end != '\0') return false;
    memcpy(body, line, blen); body[blen] = '\0';
    return evq_crc32(0, body, blen) == (uint32_t)want;
}

static bool valid_name(const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || n >= EVQ_NAME_MAX) return false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || c == '-' || c == '.' || c == '_')) return false;
    }
    return true;
}

bool evq_index_apply(evq_index_t *ix, const char *body)
{
    char kind = body[0];
    if (kind == '\0' || body[1] != ' ') return false;
    const char *a = body + 2;
    unsigned long seq = 0;
    switch (kind) {
    case 'S': {
        long long first = 0, last = 0;
        unsigned long count = 0, bytes = 0, crc = 0;
        if (sscanf(a, "%lu %lld %lld %lu %lu %lx", &seq, &first, &last, &count, &bytes, &crc) != 6) return false;
        evq_seg_t *s = evq_index_add(ix, (uint32_t)seq);
        if (s == NULL) return false;
        s->first_id = first; s->last_id = last; s->count = (uint32_t)count;
        s->bytes = (uint32_t)bytes; s->crc = (uint32_t)crc;
        s->state = EVQ_SEG_FLASH;
        s->primary[0] = s->mirror[0] = '\0';
        s->cid = 0;
        return true;
    }
    case 'P': {
        unsigned long cid = 0;
        char p[EVQ_NAME_MAX + 1], m[EVQ_NAME_MAX + 1];
        if (sscanf(a, "%lu %lx %32s %32s", &seq, &cid, p, m) != 4) return false;
        if (!valid_name(p) || !valid_name(m)) return false;
        evq_seg_t *s = evq_index_find(ix, (uint32_t)seq);
        if (s == NULL) return false;
        s->state = EVQ_SEG_SPOOLED; s->cid = (uint32_t)cid;
        memcpy(s->primary, p, strlen(p) + 1);   /* valid_name(): < EVQ_NAME_MAX */
        memcpy(s->mirror, m, strlen(m) + 1);
        return true;
    }
    case 'O': case 'D': case 'A': {
        if (sscanf(a, "%lu", &seq) != 1) return false;
        evq_seg_t *s = evq_index_find(ix, (uint32_t)seq);
        if (s == NULL) return false;
        if (kind == 'A') { evq_index_remove(ix, (uint32_t)seq); return true; }
        s->state = (kind == 'O') ? EVQ_SEG_SD_ONLY : EVQ_SEG_DELIVERED;
        return true;
    }
    case 'R': {
        unsigned long off = 0;
        long long rem = -1;
        if (sscanf(a, "%lu %lu %lld", &seq, &off, &rem) != 3) return false;
        evq_seg_t *s = evq_index_find(ix, (uint32_t)seq);
        if (s == NULL) return false;
        s->state = EVQ_SEG_REIMPORT; s->reimport_off = (uint32_t)off;
        s->reimport_remaining = rem;
        return true;
    }
    case 'W': {
        unsigned long off = 0;
        if (sscanf(a, "%lu %lu", &seq, &off) != 2) return false;
        ix->have_watermark = true; ix->wm_seq = (uint32_t)seq; ix->wm_off = (uint32_t)off;
        return true;
    }
    default:
        return false;
    }
}

/* ── persistence ────────────────────────────────────────────────────────── */

esp_err_t evq_index_load(evq_index_t *ix, const char *path)
{
    ix->n = 0; ix->lines = 0; ix->bad_lines = 0; ix->file_bytes = 0; ix->have_watermark = false;
    FILE *f = fopen(path, "rb");
    if (f == NULL) return ESP_OK;                         /* fresh: empty index */
    char line[160], body[160];
    while (fgets(line, sizeof line, f) != NULL) {
        size_t len = strlen(line);
        ix->file_bytes += len;
        if (len == sizeof line - 1 && line[len - 1] != '\n') {
            /* Over-long garbage: consume to newline, count as bad. */
            int c;
            while ((c = fgetc(f)) != EOF) { ix->file_bytes++; if (c == '\n') break; }
            ix->bad_lines++;
            continue;
        }
        if (!evq_index_parse_line(line, body, sizeof body) || !evq_index_apply(ix, body)) {
            ix->bad_lines++;
            continue;
        }
        ix->lines++;
    }
    bool err = ferror(f) != 0;
    fclose(f);
    return err ? ESP_FAIL : ESP_OK;
}

esp_err_t evq_index_append(evq_index_t *ix, const char *path, const char *fmt, ...)
{
    char body[128], line[160];
    va_list ap;
    va_start(ap, fmt);
    int bn = vsnprintf(body, sizeof body, fmt, ap);
    va_end(ap);
    if (bn < 0 || (size_t)bn >= sizeof body) return ESP_ERR_INVALID_SIZE;
    int n = evq_index_format_line(line, sizeof line, body);
    if (n < 0) return ESP_ERR_INVALID_SIZE;
    /* A new segment that the RAM table cannot hold must not reach the disk
     * either, or a later replay (with free slots) would diverge from RAM. */
    if (body[0] == 'S') {
        unsigned long seq = strtoul(body + 2, NULL, 10);
        if (evq_index_find(ix, (uint32_t)seq) == NULL && ix->n >= ix->cap) return ESP_ERR_NO_MEM;
    }

    EVQ_FAULT_POINT("index.append_torn");
    FILE *f = fopen(path, "ab");
    if (f == NULL) return ESP_FAIL;
    size_t w = fwrite(line, 1, (size_t)n, f);
    bool ok = (w == (size_t)n) && fflush(f) == 0 && fsync(fileno(f)) == 0;
    if (fclose(f) != 0) ok = false;
    if (!ok) return ESP_FAIL;
    /* Durable: fold it into RAM only now, so RAM never runs ahead of disk. */
    if (!evq_index_apply(ix, body)) return ESP_ERR_INVALID_STATE;
    ix->lines++;
    ix->file_bytes += (size_t)n;
    return ESP_OK;
}

static bool write_line(FILE *f, const char *body)
{
    char line[160];
    int n = evq_index_format_line(line, sizeof line, body);
    return n > 0 && fwrite(line, 1, (size_t)n, f) == (size_t)n;
}

esp_err_t evq_index_compact(evq_index_t *ix, const char *path, const char *tmp_path)
{
    FILE *f = fopen(tmp_path, "wb");
    if (f == NULL) return ESP_FAIL;
    bool ok = true;
    size_t bytes = 0;
    uint32_t lines = 0;
    char body[128];
    if (ix->have_watermark) {
        snprintf(body, sizeof body, "W %" PRIu32 " %" PRIu32, ix->wm_seq, ix->wm_off);
        ok = ok && write_line(f, body); lines++;
    }
    for (size_t i = 0; ok && i < ix->n; i++) {
        const evq_seg_t *s = &ix->segs[i];
        snprintf(body, sizeof body, "S %" PRIu32 " %lld %lld %" PRIu32 " %" PRIu32 " %08" PRIx32,
                 s->seq, (long long)s->first_id, (long long)s->last_id, s->count, s->bytes, s->crc);
        ok = ok && write_line(f, body); lines++;
        if (s->state != EVQ_SEG_FLASH && s->primary[0] != '\0') {
            snprintf(body, sizeof body, "P %" PRIu32 " %08" PRIx32 " %s %s",
                     s->seq, s->cid, s->primary, s->mirror);
            ok = ok && write_line(f, body); lines++;
        }
        if (s->state == EVQ_SEG_SD_ONLY || s->state == EVQ_SEG_DELIVERED) {
            snprintf(body, sizeof body, "%c %" PRIu32, s->state == EVQ_SEG_SD_ONLY ? 'O' : 'D', s->seq);
            ok = ok && write_line(f, body); lines++;
        } else if (s->state == EVQ_SEG_REIMPORT) {
            snprintf(body, sizeof body, "R %" PRIu32 " %" PRIu32 " %lld",
                     s->seq, s->reimport_off, (long long)s->reimport_remaining);
            ok = ok && write_line(f, body); lines++;
        }
    }
    if (ok) {
        long pos = ftell(f);
        bytes = pos > 0 ? (size_t)pos : 0;
    }
    ok = ok && fflush(f) == 0 && fsync(fileno(f)) == 0;
    if (fclose(f) != 0) ok = false;
    if (!ok) { remove(tmp_path); return ESP_FAIL; }
    EVQ_FAULT_POINT("compact.after_tmp_before_rename");
    EVQ_FAULT_POINT("compact.inside_rename");
    /* littlefs rename replaces the target atomically; the old index stays
     * authoritative until this returns. */
    if (rename(tmp_path, path) != 0) { remove(tmp_path); return ESP_FAIL; }
    EVQ_FAULT_POINT("compact.after_rename");
    ix->lines = lines;
    ix->bad_lines = 0;
    ix->file_bytes = bytes;
    return ESP_OK;
}
