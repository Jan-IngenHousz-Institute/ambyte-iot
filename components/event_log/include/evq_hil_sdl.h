#pragma once

/* sd_logger verification trace (Sprint 2 H2/H7/H8), verification build only
 * (CONFIG_AMBYTE_EVQ_HIL, env evq-hil) — never linked into the release image.
 * Wire format: docs/sdlog-hil-trace.md.
 *
 * Why it exists: on real hardware the only way to prove what sd_logger wrote,
 * rolled back, dropped or evicted — byte for byte, including organic log lines
 * interleaved with synthetic ones — is to record the exact framed record bytes
 * as they enter the RAM ring, and every writer decision (pop, write, commit,
 * rollback, rotation, open/close, drop), in ONE total order. The host replays
 * that order and must reproduce the card's bytes (tools/evq_hil/sdlog_check.py).
 *
 * Ordering: producer (P) and pop (POP) entries are recorded INSIDE sd_logger's
 * ring lock, so trace order == ring order even with concurrent producers;
 * writer entries come from the single writer task. The trace has its own lock,
 * always taken after the ring lock. The logger is never blocked by the trace:
 * a full trace drops the entry and counts it `lost` (and consumes its seq, so
 * the gap is visible) — evidence capacity is the host's job (rolling drains). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    HIL_SDL_WR = 1, HIL_SDL_COMMIT, HIL_SDL_RB, HIL_SDL_ROT, HIL_SDL_OPEN, HIL_SDL_CLOSE, HIL_SDL_DROP, HIL_SDL_QUI,
};
/* RB outcomes, ROT ops, CLOSE/DROP/QUI codes (the `a`/`b` argument values). */
enum { HIL_SDL_RB_OK = 0, HIL_SDL_RB_QUAR_TRUNC = 1, HIL_SDL_RB_QUAR_FSYNC = 2 };
enum { HIL_SDL_ROT_OK = 0, HIL_SDL_ROT_FAIL_REMOVE = 1, HIL_SDL_ROT_FAIL_RENAME = 2 };
enum { HIL_SDL_CLOSE_OK = 0, HIL_SDL_CLOSE_ERR = 1, HIL_SDL_CLOSE_ABANDON = 2 };
enum { HIL_SDL_DROP_ROTATE_BLOCKED = 0, HIL_SDL_DROP_UNAVAILABLE = 1 };
enum { HIL_SDL_QUI_PAUSED = 0, HIL_SDL_QUI_RESUMED = 1, HIL_SDL_QUI_TIMEOUT = 2, HIL_SDL_QUI_REFUSED = 3 };

/* 128 KiB, sized from the E8:F6:0A bench's MEASURED runtime headroom: with the
 * event store, MQTT and Wi-Fi up it has 393,228 B of PSRAM free (2 MiB part), so
 * the 512 KiB ring of replacement amendment A3 could never allocate - the
 * fail-closed check refused it on hardware (SLT_ERR noalloc free=393228).
 * 128 KiB leaves ~262 KiB free, above the 192 KiB floor (3x the publisher's
 * 64 KiB outstanding window). Host drains every 2 s: at the ~13.5 KiB/s worst
 * case the ring reaches ~27 KiB between drains, under the 25 % watermark
 * (32 KiB) and ~2.4x below the 50 % STOP line (64 KiB). */
#define HIL_SDL_CAP_BYTES (128u * 1024u)
/* `on` refuses (SLT_ERR noalloc, ring freed) unless this much PSRAM stays free
 * after the ring is allocated: tracing must not starve the store it observes. */
#define HIL_SDL_MIN_FREE_PSRAM (192u * 1024u)

/* ── control (CLI) ── */
int  hil_sdl_trace_on(bool autoarm);     /* 0 ok, -1 noalloc (prints SLT_ON / SLT_ERR) */
int  hil_sdl_trace_probe(size_t min_free);   /* G-TR: SLT_PROBE ... ok|noalloc, never records */
void hil_sdl_trace_off(void);
void hil_sdl_trace_drain(void);          /* SLT_DRAIN, SLT_E..., SLT_HDR */
void hil_sdl_trace_stat(void);
void hil_sdl_autoarm_set(void);          /* RTC one-shot for the next boot */
void hil_sdl_boot_hook(void);            /* start of sd_logger_init: honour a valid autoarm (not after power-on) */
#ifdef EVQ_HIL_HOST
extern bool hil_sdl_host_power_on;       /* host harness: simulate a power-on boot */
extern size_t hil_sdl_host_free_psram;   /* host harness: free PSRAM after an allocation */
#endif

/* ── hooks (sd_logger.c) ── */
void hil_sdl_push(const uint8_t *rec, size_t n, bool pushed);   /* caller holds the ring lock */
void hil_sdl_pop(size_t n);                                      /* caller holds the ring lock */
void hil_sdl_after_push(void);                                   /* outside the lock: watermark line */
void hil_sdl_ev(int kind, uint32_t a, uint32_t b, uint32_t c, const char *name);

/* ── H8a: synchronous pre-reset witness (writer task, just before the ROM reset) ── */
void hil_sdl_reset_witness(const char *path, uint32_t off_before, const void *chunk, size_t n, size_t half);

/* ── H7 guard interlock: the power guard's park/unpark note themselves here ── */
void     hil_sdl_guard_note(bool parked);
uint32_t hil_sdl_guard_gen(void);
bool     hil_sdl_guard_parked(void);

/* sd_logger accessors, defined in sd_logger.c under CONFIG_AMBYTE_EVQ_HIL only. */
bool sd_logger_hil_paused(void);
void sd_logger_hil_state(uint32_t *file_bytes, uint32_t *committed, bool *quar, uint32_t *backoff_ms_left, bool *open);

/* Shared helpers (also used by the evq_hil commands). */
void   hil_sdl_sha256(const void *p, size_t n, uint8_t out[32]);
void   hil_sdl_sha256_init(void *ctx);                       /* ctx = hil_sdl_sha_ctx_t */
void   hil_sdl_sha256_update(void *ctx, const void *p, size_t n);
void   hil_sdl_sha256_final(void *ctx, uint8_t out[32]);
size_t hil_sdl_b64(const uint8_t *in, size_t n, char *out, size_t cap);   /* returns length, NUL-terminated */
void   hil_sdl_hex(const uint8_t *in, size_t n, char *out);               /* out: 2n+1 */

typedef struct {
    uint32_t h[8];
    uint64_t len;
    uint8_t  buf[64];
    size_t   fill;
} hil_sdl_sha_ctx_t;

#ifdef __cplusplus
}
#endif
