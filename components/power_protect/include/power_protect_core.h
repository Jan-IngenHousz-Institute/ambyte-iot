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
 *                          or 0 < battery < 3.40 V. battery 0 with input
 *                          (the idle charger, C below) never triggers on
 *                          its own: Uluru units idle on 3.3-3.6 V rails all
 *                          day, and entering there would cut their Fv/Fm
 *                          data mid-deployment. Only the EXIT reads 0 as
 *                          "the rail is the battery".
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
 * Exit (only when not forced), field origin Uluru 17-20 Nov 2026 (solar units):
 *   the first rule, "30 min of CONTINUOUS qualifying reads", never fired on a
 *   weak panel that charges in 80-s bursts and collapses to its 4.3 V floor
 *   between them, and could not fire at all on a dead panel (14:63:93:37:A0:C4
 *   stayed in no_flash all day after one brownout). So:
 *   A. ACCUMULATED evidence. A read QUALIFIES when it is valid, input present,
 *      frames changing (power_policy's flash evidence), system ≥ 3.60 V and the
 *      battery ≥ 3.70 V. Each interval between two consecutive qualifying reads
 *      (at most 60 s apart) adds to the evidence; 30 min of it within the last
 *      12 h exits (power_recovered). Reads that carry NO evidence (invalid,
 *      stale, no_input, input absent) and read gaps PAUSE the accumulation; a
 *      fresh read with input that is LOW (below the exit thresholds) RESETS
 *      it. Evidence older than the 12-h horizon (30-min buckets) drops out.
 *      RAM only: a reboot starts it again (documented trade-off; the NVS blob
 *      is never written per read).
 *   C. battery_v == 0 with input is NOT "no battery": the charger idles with
 *      the panel present (VIN_STAT 000 / not charging) and the unit runs on the
 *      battery through the BATFET, so system_v IS the battery voltage
 *      (10:BD:A3:63:FD:20 left protection on a 3.68 V rail and browned out the
 *      same day). The board cannot tell an absent battery from an idle charger,
 *      so on the EXIT a 0 always means "judge the rail as the battery": the
 *      battery exit threshold (3.70 V) applies to system_v. Entry is
 *      unchanged (a 0 never triggers; see Entry above).
 *   B. Battery-only PROBE. On battery the ADC is dead and nothing qualifies, so
 *      a protected, automatic (not forced) unit with a live charger monitor
 *      that has seen no brownout and no fresh low read for a quiet period
 *      steps down ONE level on probation: hold -> no_flash, no_flash -> normal
 *      (reason probe). The quiet period starts at the entry (or the probe
 *      step) and at the newest brownout: 6 h, doubling per failed probe up to
 *      24 h (6 -> 12 -> 24 h). Probation lasts 24 h (one full day/night cycle,
 *      the dawn minimum included); a brownout during it is a failed probe
 *      (-> hold, reason probe_failed), as is a fresh read below the ENTRY
 *      thresholds (-> back to the level the probe left). 6 failed probes in a
 *      row exhaust the probe until a qualifying exit or clear. The failure
 *      count and the probation persist (the blob is written on the probe step,
 *      its pass and its failure: each is a transition). With no charger
 *      monitor (no reads, or the charger never answering) there is no probe:
 *      fail-closed, as before. While qualifying evidence sits inside the
 *      horizon the accumulation decides and the probe waits.
 *   Durability (review of PR #113, rounds 1-2): ONE generation-ordered record.
 *    - Every persisted record carries a generation number (gen) that is never
 *      reused. The glue keeps two copies: NVS, and an RTC_NOINIT mirror (two
 *      slots, bound to the NVS epoch) that survives resets without power loss.
 *      With the epoch unreadable the glue is passive (U1, pp_unverified_floor)
 *      until it reads (U2, pp_on_verified). At boot the NEWEST valid copy by
 *      generation wins (the RTC copy only after a reset without power loss);
 *      neither copy is consumed by being read.
 *    - A protective update (entry, escalation, a failed probe, a brownout or
 *      low-read record, a force up) is applied at once, written to the RTC
 *      mirror, then to NVS. If NVS refuses, the RTC copy is the newest record
 *      and every later reset without power loss adopts it until NVS takes a
 *      newer one.
 *    - A RELAXATION (the probe step, a passed probation, the accumulated
 *      exit; and an operator clear or force down) is computed on a copy and
 *      written to NVS FIRST; only then does it replace the state and the RTC
 *      mirror. pp_on_read never applies an automatic relaxation itself: it
 *      applies the read's protective facts (low veto, evidence, failures) and
 *      reports the relaxation as due (relax_due); the glue applies it with
 *      pp_relax on the copy. A refused write leaves the state with every
 *      protective fact of the read, and no relaxation is tried for
 *      PP_RELAX_RETRY_MS. The refused generation number is consumed.
 *    - A probation found at boot after a POWER loss (power-on, power glitch,
 *      brownout) is a failed probe: the run on probation ended with the power
 *      gone. After any other reset (the nightly reboot, a watchdog) it goes on.
 *    - The newest fresh low read is part of the record (has_low, low_utc):
 *      the first of a state, the first one the clock can date (or, with the
 *      clock no longer trusted, the switch to undated), then at most once per
 *      PP_LOW_REFRESH_S. After a reboot a dated one counts PP_LOW_REFRESH_S
 *      later than written (it may be that stale); an undated one makes the
 *      quiet period count from boot.
 * Operator: force(no_flash|hold) sets a forced state that never auto-exits
 *   (a linked brownout still escalates forced no_flash to hold); clear returns
 *   to normal and wipes the brownout history, the forced flag, the probation
 *   and the backoff level.
 *
 * Persistence: the pp_persist_t below, as a versioned, CRC-checked blob. The
 * caller writes it ONLY when an event says persist (state transitions, a
 * recorded brownout — at most one per boot — a probe step, its pass or
 * failure, force and clear); reads and heartbeats never write. An ABSENT blob
 * with no NVS epoch (first install) starts at normal; absent beside an epoch
 * the state was lost (pp_load_missing: no_flash, state_missing); an
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
/* Exit thresholds: every qualifying read must meet them. Above the entry ones
 * so a rail sitting at the edge cannot flap in and out. */
#define PP_SYS_EXIT_MV      3600u
#define PP_BAT_EXIT_MV      3700u
/* Qualifying evidence needed to exit. Unchanged from the continuous rule: 30
 * min of charger-confirmed healthy rail and battery. What changed is that it
 * may be collected in pieces (A above). */
#define PP_EXIT_TARGET_MS   (30LL * 60 * 1000)
/* Horizon the evidence must fall inside. 12 h = one daylight period: evidence
 * from yesterday afternoon is separated from this morning by a night on
 * battery the ADC cannot see, so it must not count. Anything older drops out
 * in 30-min buckets (evidence 11.5-12 h old may already have gone). A battery
 * that sagged overnight is caught anyway: the first fresh low read in the
 * morning resets the whole accumulation. */
#define PP_EXIT_HORIZON_MS  (12LL * 60 * 60 * 1000)
#define PP_EXIT_BUCKET_MS   (30LL * 60 * 1000)
#define PP_EXIT_BUCKETS     ((unsigned)(PP_EXIT_HORIZON_MS / PP_EXIT_BUCKET_MS))
/* Only the interval between two consecutive qualifying reads counts, and only
 * up to this long: device_commands reads the charger at most every 5 s on
 * demand and the power guard task asks every 15 s, so 60 s without a read
 * means nobody was looking. That interval earns nothing (the accumulation
 * pauses); it no longer throws away what was already collected. */
#define PP_READ_GAP_MAX_MS  (60LL * 1000)
#define PP_BROWNOUT_LINK_S  (6LL * 60 * 60)
#define PP_HIST_MAX         8u

/* Battery-only probe (B above). 6 h base quiet period: past the 6-h brownout
 * link window, so the episode that put the unit here is over by the linking
 * rule's own measure, and long enough that one probe per failure costs at most
 * one brownout per 6/12/24 h. The level doubles per failed probe and caps at
 * 24 h (one probe a day at most). 24 h of probation covers one full
 * day/night cycle including the pre-dawn battery minimum, when the MPF slots
 * hit the weakest battery; a shorter one would "pass" in the evening and fail
 * at dawn as a fresh, unlinked episode, i.e. one silent brownout a day. 6
 * failures in a row (6+12+24+24+24+24 h ≈ 5 days of trying) stop the probe:
 * every failure is a power-on-class reset of a depleted battery (the SD has
 * no journal), so the attempts are bounded per episode. */
#define PP_PROBE_QUIET_BASE_S  (6LL * 60 * 60)
#define PP_PROBE_LEVEL_CAP     2u      /* 6 h << 2 = 24 h */
#define PP_PROBE_FAILS_MAX     6u
#define PP_PROBATION_S         (24LL * 60 * 60)
/* A relaxation whose NVS commit failed is not retried for an hour: at most
 * one write attempt per hour while the NVS refuses, and the unit stays
 * protected in between. */
#define PP_RELAX_RETRY_MS      (60LL * 60 * 1000)
/* The durable low-read time is refreshed at most hourly while fresh low reads
 * continue; never per read. Real bound per protected state: 1 (the first low)
 * + ≤ 1 an hour (≤ 24 a day) + ≤ 2 per change of clock trust (undate,
 * re-date). The cost is that after a reboot a dated veto counts from up to an
 * hour later than the real last low read. */
#define PP_LOW_REFRESH_S       (60LL * 60)

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
    PP_REASON_PROBE,            /* "probe": battery-only probe, one level down on probation */
    PP_REASON_PROBE_FAILED,     /* "probe_failed": a brownout (or fresh low read) on probation */
    PP_REASON_STATE_MISSING,    /* "state_missing": NVS has an epoch but no record: the state was lost */
    PP_REASON_IDENTITY_UNVERIFIED, /* "identity_unverified": the NVS epoch is unreadable: failed closed (U1) */
    PP_REASON_COUNT
} pp_reason_t;
/* "probe_passed" is a log reason only (the probation ended without a failure;
 * the state keeps reason probe, it did not change). */

/* A probe in flight (persisted): which step is on probation. */
typedef enum {
    PP_PROBATION_NONE = 0,
    PP_PROBATION_NO_FLASH = 1,   /* hold -> no_flash */
    PP_PROBATION_NORMAL = 2,     /* no_flash -> normal */
} pp_probation_t;

/* The probe as reported (health.power_protect.probe). */
typedef enum {
    PP_PROBE_OFF = 0,     /* "off": normal (not on probation), or a forced state */
    PP_PROBE_WAITING,     /* "waiting": protected, counting the quiet period */
    PP_PROBE_PROBATION,   /* "probation" */
    PP_PROBE_EXHAUSTED,   /* "exhausted": PP_PROBE_FAILS_MAX failures; qualifying exit or clear */
    PP_PROBE_NO_MONITOR,  /* "no_monitor": no charger read answered: fail-closed */
} pp_probe_t;

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
    /* Blob v2. The probe step on probation (pp_probation_t; its start is
     * since_utc, the step being a state change) and the failed probes since
     * the last qualifying exit or clear (the backoff level). */
    uint8_t  probation;
    uint8_t  probe_fails;
    /* The newest fresh low read of this state (cleared by every transition):
     * has_low with low_utc 0 = seen while the clock was not valid. */
    bool     has_low;
    int64_t  low_utc;
    /* Generation of this record: the glue numbers every write, never reusing
     * a number; at boot the newer of the NVS and RTC copies wins. */
    uint32_t gen;
} pp_persist_t;

typedef struct {
    pp_persist_t p;
    /* RAM only: the accumulated exit evidence, ms per 30-min bucket of the
     * monotonic clock (slot = bucket % PP_EXIT_BUCKETS, tagged with its
     * bucket number so a stale slot is never summed). */
    uint32_t acc_ms[PP_EXIT_BUCKETS];
    int64_t  acc_bucket[PP_EXIT_BUCKETS];
    bool     prev_qual;        /* the previous read qualified */
    bool     have_read;
    int64_t  last_read_ms;     /* monotonic, last read */
    bool     have_low;         /* a fresh low read this boot (blocks the probe) */
    int64_t  last_low_ms;
    bool     monitor_live;     /* the last read came from a charger that answered */
    int64_t  since_mono_ms;    /* monotonic time of this boot's last read-driven transition; 0 = boot */
    int64_t  relax_hold_until_ms; /* no relaxation before this (a failed commit); glue-owned */
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
    bool        probe_passed;   /* probation ended well (one E line, state unchanged) */
    bool        relaxes;        /* this (already applied) event lowers protection: the caller ran it on a
                                 * copy and must commit it to NVS BEFORE adopting it */
    uint8_t     relax_due;      /* pp_relax_t: an automatic relaxation is due (NOT applied) */
} pp_event_t;

typedef enum {
    PP_RELAX_NONE = 0,
    PP_RELAX_EXIT,    /* accumulated evidence: -> normal (power_recovered) */
    PP_RELAX_PROBE,   /* one level down on probation (probe) */
    PP_RELAX_PASS,    /* probation passed (probe_passed) */
} pp_relax_t;

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

/* NVS has an epoch (so this module wrote a record into it) but no record:
 * the protection state was lost (review of PR #113, round 5). Like
 * unreadable, but reason state_missing. Only no epoch AND no record is a
 * first install (normal). */
void pp_load_missing(pp_core_t *c, pp_clock_t clk);

/* Protection order of a record: hold > no_flash > normal, a forced state above
 * the automatic one of the same level. For comparing records whose
 * generations cannot be compared (another NVS lifetime). */
static inline int pp_protection_rank(const pp_persist_t *p)
{
    return (int)p->state * 2 + (p->forced ? 1 : 0);
}

/* U1 (review of PR #113, round 7): the NVS epoch exists but cannot be read,
 * so nothing proves which RTC record, if any, belongs to this NVS lifetime.
 * The unit fails closed and stays passive: the state is at least no_flash
 * (reason identity_unverified when raised; a forced NVS state is kept), RTC is
 * neither read nor written, NVS is not written, and no relaxation runs. RAM
 * only. */
void pp_unverified_floor(pp_core_t *c, pp_clock_t clk);

/* U2: the epoch has just been read, after a U1 period. `selected` is the
 * VERIFIED selection (NVS against same-lifetime RTC slots, by generation).
 * The result, conservatively: (a) a probation in it counts as a FAILED probe
 * (hold, backoff +1: the unverified period hid what happened); (b) the quiet
 * period restarts now (a dated low veto at the verification time); (c) the
 * state is raised to at least the RAM state of the U1 period (state rank and
 * forced only). Returns a persisting event (the caller writes it RTC-first). */
pp_event_t pp_on_verified(pp_core_t *c, const pp_persist_t *selected, int64_t mono_ms, pp_clock_t clk);

/* Blob codec. encode always writes v2 (PP_BLOB_LEN). decode reads v2 and the
 * v1 blob of the first brownout-protection release (PP_BLOB_V1_LEN, no probe
 * fields: none on probation, no failures), so an upgrade never turns a stored
 * state into "unreadable". It validates version, length, CRC and every range;
 * false leaves *out untouched. A rollback to v1 firmware reads a v2 blob as
 * unreadable (no_flash, state_unreadable): the safe side. */
#define PP_BLOB_VERSION 2u
#define PP_BLOB_V1_LEN  (1u + 3u + 8u + 1u + PP_HIST_MAX * 12u + 4u)
#define PP_BLOB_LEN     (PP_BLOB_V1_LEN + 2u + 1u + 8u + 4u)
size_t pp_encode(const pp_persist_t *p, uint8_t *buf, size_t cap);
bool   pp_decode(pp_persist_t *out, const uint8_t *buf, size_t len);

/* Once per boot, after the persisted state is loaded and before anything that
 * sheds runs. Idempotent per boot number. */
pp_event_t pp_on_boot(pp_core_t *c, bool brownout_reset, uint32_t boot, pp_clock_t clk);

/* This boot followed a power loss other than a brownout (power-on, power
 * glitch). A probation in flight failed (-> hold, probe_failed); otherwise
 * nothing. Call before pp_on_boot. */
pp_event_t pp_on_power_loss(pp_core_t *c, pp_clock_t clk);

/* One charger read: the driver verdict, the reading (only when OK), and
 * power_policy's fresh-external-power evidence (power_gate_flash_ok after this
 * read was stepped in). mono_ms is monotonic time SINCE BOOT (esp_timer): the
 * probe falls back to uptime when the clock cannot date the quiet period. The
 * accumulation, the probe and the probation are all decided here. */
pp_event_t pp_on_read(pp_core_t *c, power_read_reason_t reason, const power_reading_t *p,
                      bool fresh_present, int64_t mono_ms, pp_clock_t clk);

/* Apply a relaxation pp_on_read reported as due. Call it on a COPY of the
 * core, commit the copy's record to NVS, and adopt the copy only once that
 * write succeeded. The event has relaxes set. */
pp_event_t pp_relax(pp_core_t *c, pp_relax_t what, int64_t mono_ms, pp_clock_t clk);

/* Operator commands. An event with relaxes set (clear; force to a lower
 * state) must be run on a copy and committed to NVS first, like pp_relax. */
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

/* Qualifying evidence still needed to exit, in ms; -1 when none has been
 * collected (or the state cannot exit by itself: normal, forced). */
int64_t pp_exit_remaining_ms(const pp_core_t *c, int64_t mono_ms);

/* Recovery progress for reporting. */
typedef struct {
    uint32_t   qualifying_s;   /* accumulated evidence inside the horizon */
    pp_probe_t probe;
    int64_t    next_probe_s;   /* PP_PROBE_WAITING only: seconds to the probe; -1 = n/a */
    uint8_t    backoff_level;  /* failed probes since the last qualifying exit / clear */
} pp_recovery_t;
pp_recovery_t pp_recovery(const pp_core_t *c, int64_t mono_ms, pp_clock_t clk);

static inline bool pp_blocks_flash(pp_state_t s) { return s != PP_STATE_NORMAL; }
static inline bool pp_blocks_ambit(pp_state_t s) { return s == PP_STATE_HOLD; }

const char *pp_state_name(pp_state_t s);
const char *pp_reason_name(pp_reason_t r);
const char *pp_probe_name(pp_probe_t p);
/* "no_flash" / "hold" only (the force targets); false otherwise. */
bool pp_force_state_parse(const char *s, pp_state_t *out);

/* health.power_protect object: {"state","reason","since_utc"|null,"forced",
 * "persist_fault","brownouts_6h","qualifying_s","probe","next_probe_s"|null,
 * "backoff_level","skipped":{"flash_runs","ambit_runs","ota"}}. rec NULL =
 * nothing collected, probe off. Returns the length, or -1 when it does not fit
 * (nothing partial is ever returned). Worst case ~330 B. */
#define PP_JSON_CAP 384u
int pp_render_json(char *buf, size_t cap, const pp_persist_t *p, bool persist_fault,
                   uint32_t brownouts_6h, const pp_skips_t *skips, const pp_recovery_t *rec);

#ifdef __cplusplus
}
#endif
