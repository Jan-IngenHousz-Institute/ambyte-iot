#pragma once
/*
 * power_protect_core.h — the brownout protection state machine, as pure C
 * over (boot facts, charger reads, operator commands, a clock sample). No NVS,
 * no FreeRTOS, no logging, no clock reads: power_protect.c owns those, and
 * tests/power_protect_host.c drives this exact file. See docs/power-protect.md.
 *
 * WHY (field evidence, 2026-11):
 *  - 14 Nov: Uluru units on firmware without diagnostics reset exactly at the
 *    every-10-minute MPF slots, when all four AMBITs fire saturating flashes at
 *    once, and then went silent; units with input present but a sagging rail
 *    (2.7–2.9 V) or a 0 V battery kept rebooting.
 *  - 15 Nov: v2.18.0's reset reasons gave the first direct proof: 14:63:93:39:36:48,
 *    battery only, reported reset_reason=brownout on boots 2, 3 and 4 (07:00,
 *    07:13, 07:18 ACST) and each time restarted straight back into the same load.
 * Nothing reacted. The user's requirement: detect it, stop the risky work,
 * report it, and hold the actions until the power status changes.
 *
 * WHAT IT CAN SEE: on battery alone the MP2731 ADC does not convert (datasheet
 * p.26, docs/power-telemetry.md), so there is no voltage to predict a brownout
 * with. The machine therefore REACTS to the first brownout (esp_reset_reason)
 * and, only where the charger gives fresh evidence (input present), to a low
 * rail or battery.
 *
 * States:
 *   normal   – nothing shed.
 *   no_flash – actinic-bearing AMBIT work, AMBIT flashing and every OTA shed.
 *   hold     – every ambit/ action shed as well (heartbeat, MQTT and the
 *              command path keep running).
 *
 * Entry:
 *   normal   -> no_flash : this boot's reset reason is brownout, or a FRESH
 *                          valid read with input present shows system < 3.30 V
 *                          or 0 < battery < 3.40 V (battery 0 with input = no
 *                          battery / idle charger, never a trigger).
 *   *        -> hold     : a brownout LINKED to the previous one (below).
 * Linking two brownouts (brownout history, persisted):
 *   - both carry a valid UTC (the clock was valid at both boots) and the new
 *     one is 0 ≤ Δ < 6 h after the previous → linked; Δ ≥ 6 h → not linked;
 *   - otherwise (either clock invalid, or the clock went backwards) fall back
 *     to "the previous boot also ended in a brownout": the previous brownout
 *     was recorded on boot_count − 1;
 *   - and when even that is impossible (a boot_count is 0: diag's counter was
 *     unavailable) the order is UNKNOWN: a unit already protected treats it as
 *     linked (hold, reason brownout_unordered); a normal one enters no_flash.
 *     Without this, battery units with no clock and no counter would brown out
 *     in no_flash forever (review of PR #103).
 * Exit (only when not forced):
 *   protected -> normal after 30 min of CONTINUOUS qualifying reads: valid,
 *   input present, frames changing (power_policy's flash evidence), system
 *   ≥ 3.60 V and (battery ≥ 3.70 V or battery == 0). Any other read, or a gap
 *   of more than 60 s without reads, restarts the window. hold therefore
 *   never exits on battery. The window lives in RAM: a reboot restarts it.
 * Operator: force(no_flash|hold) sets a forced state that never auto-exits
 *   (a linked brownout still escalates forced no_flash to hold); clear returns
 *   to normal and wipes the brownout history and the forced flag.
 *
 * Persistence: the pp_persist_t below, as a versioned, CRC-checked blob. The
 * caller writes it ONLY when an event says persist (state transitions, a
 * recorded brownout — at most one per boot — force and clear); reads and
 * heartbeats never write. An ABSENT blob (first install) starts at normal; an
 * UNREADABLE one (corrupt, or the read itself failed) starts at no_flash,
 * reason state_unreadable, with persist_fault set (pp_load_unreadable): a
 * corrupted forced hold must not come back at full load. The next persisting
 * event repairs the blob.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sensing_port.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Entry thresholds (fresh read, input present). */
#define PP_SYS_TRIGGER_MV   3300u
#define PP_BAT_TRIGGER_MV   3400u
/* Exit thresholds, held for the whole window. Above the entry ones so a rail
 * sitting at the edge cannot flap in and out. */
#define PP_SYS_EXIT_MV      3600u
#define PP_BAT_EXIT_MV      3700u
#define PP_EXIT_WINDOW_MS   (30LL * 60 * 1000)
/* A window needs CONTINUOUS evidence. device_commands reads the charger every
 * 5 s and the power guard task asks at least every 15 s; 60 s without a read
 * means nobody was looking, which is not evidence of anything. */
#define PP_READ_GAP_MAX_MS  (60LL * 1000)
#define PP_BROWNOUT_LINK_S  (6LL * 60 * 60)
#define PP_HIST_MAX         8u

typedef enum {
    PP_STATE_NORMAL = 0,
    PP_STATE_NO_FLASH = 1,
    PP_STATE_HOLD = 2,
} pp_state_t;

/* Why the current state was entered. Stable telemetry strings (never rename). */
typedef enum {
    PP_REASON_NONE = 0,         /* "none": never protected (or blob unreadable) */
    PP_REASON_BROWNOUT,         /* "brownout" */
    PP_REASON_REPEAT_BROWNOUT,  /* "repeat_brownout": linked second brownout */
    PP_REASON_LOW_VOLTAGE,      /* "low_voltage": fresh low rail/battery with input */
    PP_REASON_FORCED,           /* "forced": operator power_protect force */
    PP_REASON_POWER_RECOVERED,  /* "power_recovered": the 30-min window passed */
    PP_REASON_CLEARED,          /* "cleared": operator power_protect clear */
    PP_REASON_STATE_UNREADABLE, /* "state_unreadable": the persisted state could not be read */
    PP_REASON_BROWNOUT_UNORDERED, /* "brownout_unordered": a further brownout, order unknown */
    PP_REASON_COUNT
} pp_reason_t;

typedef struct {
    int64_t  utc;    /* UTC seconds when recorded; 0 = the clock was not valid */
    uint32_t boot;   /* diag boot_count of the brownout boot; 0 = unknown */
} pp_brownout_t;

/* Everything that survives a reboot. */
typedef struct {
    uint8_t  state;      /* pp_state_t */
    uint8_t  reason;     /* pp_reason_t */
    bool     forced;
    int64_t  since_utc;  /* entry time of `state`; 0 = clock not valid then */
    uint8_t  hist_n;     /* entries in hist, oldest first */
    pp_brownout_t hist[PP_HIST_MAX];
} pp_persist_t;

typedef struct {
    pp_persist_t p;
    /* RAM only: the exit window. */
    bool     window_open;
    int64_t  window_start_ms;  /* monotonic */
    bool     have_read;
    int64_t  last_read_ms;     /* monotonic, last read seen while protected */
    /* The NVS copy is not trustworthy: unreadable at boot, or the last write
     * failed. Cleared by the next successful write (the glue owns it). */
    bool     persist_fault;
} pp_core_t;

/* A clock sample: valid = trustworthy UTC (see power_protect.c). */
typedef struct {
    bool    valid;
    int64_t utc;
} pp_clock_t;

/* What one input did. `persist`: write the blob now. `changed`: the state
 * moved from `from` to `to` (one ESP_LOGE line). */
typedef struct {
    bool        changed;
    bool        persist;
    pp_state_t  from, to;
    pp_reason_t reason;
} pp_event_t;

/* Skipped-work counters (RAM, per boot: counting into NVS would write per run). */
typedef struct {
    uint32_t flash_runs;  /* jobs shed because they carried actinic/flash work */
    uint32_t ambit_runs;  /* jobs shed by hold only (no actinic content) */
    uint32_t ota;         /* refused Ambyte OTA, AMBIT OTA, ambit_flash, boot sync */
} pp_skips_t;

void pp_core_init(pp_core_t *c);

/* The persisted state exists but could not be read: no_flash, reason
 * state_unreadable, not forced, empty history, persist_fault set. RAM only. */
void pp_load_unreadable(pp_core_t *c, pp_clock_t clk);

/* Blob codec. Encoded size is fixed: PP_BLOB_LEN. decode validates version,
 * length, CRC and every range; false leaves *out untouched. */
#define PP_BLOB_VERSION 1u
#define PP_BLOB_LEN     (1u + 3u + 8u + 1u + PP_HIST_MAX * 12u + 4u)
size_t pp_encode(const pp_persist_t *p, uint8_t *buf, size_t cap);
bool   pp_decode(pp_persist_t *out, const uint8_t *buf, size_t len);

/* Once per boot, after the persisted state is loaded and before anything that
 * sheds runs. Idempotent per boot number. */
pp_event_t pp_on_boot(pp_core_t *c, bool brownout_reset, uint32_t boot, pp_clock_t clk);

/* One charger read: the driver verdict, the reading (only when OK), and
 * power_policy's fresh-external-power evidence (power_gate_flash_ok after this
 * read was stepped in). mono_ms is monotonic. */
pp_event_t pp_on_read(pp_core_t *c, power_read_reason_t reason, const power_reading_t *p,
                      bool fresh_present, int64_t mono_ms, pp_clock_t clk);

pp_event_t pp_force(pp_core_t *c, pp_state_t state, pp_clock_t clk);
pp_event_t pp_clear(pp_core_t *c, pp_clock_t clk);

/* Do two brownouts belong to one episode (the rule above)? */
typedef enum {
    PP_LINK_NO = 0,
    PP_LINK_YES,
    PP_LINK_UNORDERED,   /* neither UTC nor boot_count can order them */
} pp_link_t;
pp_link_t pp_brownout_link(const pp_brownout_t *prev, const pp_brownout_t *cur);

/* Recorded brownouts in the last 6 h. An entry with a valid UTC, read with a
 * valid `clk`, counts when it is within 6 h. Otherwise, with both boot counts
 * known, it counts when it belongs to the run of consecutive brownout boots
 * ending at `cur_boot` (the escalation fallback). An entry whose age cannot be
 * established at all (no UTC basis, a boot_count of 0) counts: conservative,
 * until `clear` wipes the history. */
uint32_t pp_brownouts_6h(const pp_core_t *c, pp_clock_t clk, uint32_t cur_boot);

/* Remaining ms of a running exit window; -1 when none runs. */
int64_t pp_window_remaining_ms(const pp_core_t *c, int64_t mono_ms);

static inline bool pp_blocks_flash(pp_state_t s) { return s != PP_STATE_NORMAL; }
static inline bool pp_blocks_ambit(pp_state_t s) { return s == PP_STATE_HOLD; }

const char *pp_state_name(pp_state_t s);
const char *pp_reason_name(pp_reason_t r);
/* "no_flash" / "hold" only (the force targets); false otherwise. */
bool pp_force_state_parse(const char *s, pp_state_t *out);

/* health.power_protect object: {"state","reason","since_utc"|null,"forced",
 * "persist_fault","brownouts_6h","skipped":{"flash_runs","ambit_runs","ota"}}.
 * Returns the length, or -1 when it does not fit (nothing partial is ever
 * returned). */
#define PP_JSON_CAP 256u
int pp_render_json(char *buf, size_t cap, const pp_persist_t *p, bool persist_fault,
                   uint32_t brownouts_6h, const pp_skips_t *skips);

#ifdef __cplusplus
}
#endif
