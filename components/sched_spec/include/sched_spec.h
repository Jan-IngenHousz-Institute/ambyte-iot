#ifndef AMBYTE_SCHED_SPEC_H
#define AMBYTE_SCHED_SPEC_H

/*
 * sched_spec — annotated-schedule YAML → bounded, validated sched_program_t.
 *
 * Pure C11. No ESP-IDF dependency beyond esp_err_t (host-tested against
 * tests/host_stubs); the only real dependency is time_sync, itself pure.
 * The device parses the authored, commented YAML directly (design decision 2):
 * this component owns the strict subset grammar, the compiler that enforces
 * every schedule rule, and the cron/window/due math the runner needs.
 *
 * Time model: all wall-clock math runs on LOCAL Unix seconds, exactly like
 * time_sync. The component never reads a clock and never touches
 * components/timezone: the UTC→local offset arrives as a parameter (host CLI)
 * or via the localize function pointer the runner injects (sched_due).
 * Sunrise/sunset go through time_sync_sun_on_date, which applies the offset
 * the firmware already pushed into time_sync.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "time_sync.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── caps (every one carries its why; see plan "Program representation") ── */

#define SCHED_SPEC_MAX_JOBS       16   /* shipped schedules use 5 and 3 */
#define SCHED_SPEC_MAX_STEPS      8    /* no shipped job needs more than 1 */
#define SCHED_SPEC_MAX_TRIGGERS   8    /* dark_edge/health use 2–3 */
#define SCHED_SPEC_MAX_PROTOCOLS  8    /* shipped schedules use 2 and 0 */
#define SCHED_SPEC_MAX_SEGMENTS   16   /* AMBIT run arrays accept 1..16 (MAX_ARR_LEN) */
#define SCHED_SPEC_MAX_EVENT_KEYS 16   /* legacy status event has 11 data + 5 metadata */
#define SCHED_SPEC_MAX_CHANNELS   4    /* the box carries at most 4 AMBITs */
#define SCHED_SPEC_MAX_INPUTS     8    /* widest catalog action (ambit/trace) has 5 */
/* Flat typed-value pool shared by all steps. Fixed regardless of how the
 * 16×8 job×step caps are used, so one db/store-event-heavy schedule cannot
 * blow the static budget: 256 × 16 B = 4 KiB. Shipped schedules use < 40. */
#define SCHED_SPEC_MAX_ENTRIES    256
#define SCHED_SPEC_STRING_POOL    4096 /* names, tags, messages; shipped use < 400 B */

/* YAML subset limits (plan "The YAML subset"): the file lives on littlefs and
 * is parsed in a transient heap arena on a 512 KiB SRAM part, so the arena
 * worst case (nodes + strings) must stay in the low tens of KiB. */
#define SCHED_YAML_MAX_FILE_BYTES   (16 * 1024)
#define SCHED_YAML_MAX_LINE         256
#define SCHED_YAML_MAX_NODES        512
#define SCHED_YAML_MAX_STRING_BYTES (8 * 1024)
#define SCHED_YAML_MAX_DEPTH        6

#define SCHED_POOL_NONE 0xFFFFu /* pool offset sentinel: string absent */

/* The only accepted document header value (JII measurement-device standard
 * idiom: namespaced const + id/version provenance). */
#define SCHED_SPEC_SCHEMA_CONST "jii.ambyte-schedule/v1-draft"

/* ── parser output: generic node tree (transient heap arena) ─────────── */

typedef enum {
    SCHED_NODE_MAP,
    SCHED_NODE_SEQ,
    SCHED_NODE_SCALAR,
} sched_node_kind_t;

/* Scalars resolve in exactly this order (plan table): quoted string →
 * true/false → int → float → duration → HH:MM → plain string. */
typedef enum {
    SCHED_SCAL_STR,         /* quoted or plain; text in .str */
    SCHED_SCAL_BOOL,        /* .b */
    SCHED_SCAL_INT,         /* .i */
    SCHED_SCAL_FLOAT,       /* .f */
    SCHED_SCAL_DURATION_MS, /* .ms (1m → 60000) */
    SCHED_SCAL_HHMM,        /* .hh/.mm, ranges validated */
} sched_scal_kind_t;

typedef struct sched_node sched_node_t;

typedef struct {
    const char  *key;    /* arena-owned */
    sched_node_t *value;
} sched_pair_t;

struct sched_node {
    uint8_t  kind;      /* sched_node_kind_t */
    uint8_t  scal_kind; /* sched_scal_kind_t when kind == SCHED_NODE_SCALAR */
    uint16_t line, col; /* 1-based; carried into compiler error messages */
    union {
        struct {
            const char *str; /* raw text (quotes stripped) */
            int64_t     i;
            double      f;
            int64_t     ms;
            int         b;
            int         hh, mm;
        } s;
        struct { sched_pair_t  *pairs; int count; } m; /* insertion order */
        struct { sched_node_t **items; int count; } q;
    } u;
};

typedef struct sched_yaml_doc sched_yaml_doc_t; /* opaque: arena + root */

/* Parse the subset. On error returns ESP_FAIL and fills err with
 * "line:col: message". *out is a single arena handle; sched_yaml_free()
 * releases everything. */
esp_err_t sched_yaml_parse(const char *text, size_t len,
                           sched_yaml_doc_t **out, char *err, size_t err_cap);
const sched_node_t *sched_yaml_root(const sched_yaml_doc_t *doc);
void sched_yaml_free(sched_yaml_doc_t *doc);

/* ── action catalog (schema source of truth, design decision 3) ──────── */

typedef enum {
    SCHED_IN_INT,         /* range-checked via min/max */
    SCHED_IN_FLOAT,
    SCHED_IN_BOOL,
    SCHED_IN_STRING,
    SCHED_IN_DURATION_MS, /* duration scalar; range in ms via min/max */
    SCHED_IN_CHANNELS,    /* flow seq of ints 0..3, unique */
    SCHED_IN_MAP,         /* flat string→scalar map, ≤ SCHED_SPEC_MAX_EVENT_KEYS */
} sched_input_type_t;

typedef struct {
    const char *name;
    uint8_t     type;     /* sched_input_type_t */
    uint8_t     required;
    uint8_t     has_default;
    int64_t     min, max; /* INT and DURATION_MS only */
    int64_t     def_i;    /* default for INT/DURATION_MS/BOOL */
    const char *def_s;    /* default for STRING (NULL = no default) */
} sched_input_decl_t;

typedef struct sched_action sched_action_t;
struct sched_step;    /* defined below; run() is bound by the runner (T3) */
struct sched_program;
struct sched_action {
    const char               *name; /* e.g. "ambit/trace" */
    const sched_input_decl_t *inputs;
    uint8_t                   input_count;
    /* Bound by the runner (T3); NULL in this component's table. */
    void                    *run_ctx;
    esp_err_t              (*run)(void *ctx, const struct sched_step *step,
                                  const struct sched_program *prog);
};

const sched_action_t *sched_actions_table(size_t *count);
const sched_action_t *sched_action_find(const char *name);

/* The runner (T3) binds the run function + context for every catalog action
 * at start; a NULL run means a declarations-only build (host tools, CI
 * schema dump). ESP_ERR_NOT_FOUND for an unknown name. */
esp_err_t sched_action_bind(const char *name, void *run_ctx,
                            esp_err_t (*run)(void *ctx,
                                             const struct sched_step *step,
                                             const struct sched_program *prog));

/* JSON Schema draft-07 fragment set: one oneOf branch per action. Returns the
 * would-be length (snprintf semantics); truncation ⇒ return ≥ cap.
 * Size probe: buf may be NULL with cap == 0 — nothing is written, the return
 * is the required length (without NUL). */
size_t sched_actions_dump_json(char *buf, size_t cap);

/* db/store-event value placeholders resolved on device (design catalog row). */
bool sched_is_placeholder(const char *s); /* "$deployment" … "$job.fail_streak" */

/* ── compiled program ────────────────────────────────────────────────── */

/* One `with` value (or one map entry), typed, strings as pool offsets so the
 * static runner instance is relocation-free. 16 B each. */
typedef enum {
    SCHED_VAL_INT,
    SCHED_VAL_FLOAT,
    SCHED_VAL_BOOL,   /* stored in u.i as 0/1 */
    SCHED_VAL_STR,    /* u.str_off */
    SCHED_VAL_CHANNELS, /* u.chans; n == 0 = all channels that answer a ping (input absent) */
} sched_val_type_t;

typedef struct {
    uint8_t  input_idx; /* index into the action's declared inputs */
    uint8_t  type;      /* sched_val_type_t */
    uint16_t key_off;   /* map key pool offset; SCHED_POOL_NONE for scalar inputs */
    union {
        int64_t  i;
        double   f;
        uint16_t str_off;
        struct { uint8_t n; uint8_t v[SCHED_SPEC_MAX_CHANNELS]; } chans;
    } u;
} sched_entry_t;

/* AMBIT segment: all six fields the run array takes (T1 owns the matching
 * firmware struct). */
typedef struct {
    uint16_t pulses;      /* 1..65535, required */
    uint16_t freq;        /* Hz, 1..65535, required */
    int16_t  actinic;     /* WRENCH: -255..-1 raw DAC, 0 off, 1..9999 PAR µmol */
    uint8_t  type;        /* 0 skip, 1 incl. 730 nm reflectance, 2 no IR; default 2 */
    uint8_t  far_red;     /* only meaningful with type 1; default false */
    uint8_t  subsampling; /* 0 none, 1 every pulse, 2 every-8 averaged; default 1 */
    uint8_t  _pad;
} sched_segment_t;

typedef struct {
    uint16_t name_off;
    uint8_t  segment_count;
    uint8_t  persist;         /* run-level bool, default false */
    uint8_t  allow_interrupt; /* run-level bool, default false */
    uint8_t  _pad;
    sched_segment_t segments[SCHED_SPEC_MAX_SEGMENTS];
} sched_protocol_t;

/* 5-field cron as bitmasks (plan "Cron matcher"). */
typedef struct {
    uint64_t min;   /* bits 0..59 */
    uint32_t hour;  /* bits 0..23 */
    uint32_t dom;   /* bits 1..31 */
    uint16_t month; /* bits 1..12 */
    uint8_t  dow;   /* bits 0..6, 0=Sunday (7 folded in at parse) */
    uint8_t  dom_restricted; /* field did not start with '*' (vixie OR rule) */
    uint8_t  dow_restricted;
    uint8_t  _pad;
} sched_cron_t;

typedef enum {
    SCHED_TRIG_EVERY,
    SCHED_TRIG_CRON,
    SCHED_TRIG_AT,
    SCHED_TRIG_WEEKLY,
    SCHED_TRIG_SUN,
    SCHED_TRIG_BOOT,
    SCHED_TRIG_DISPATCH,
} sched_trigger_kind_t;

typedef struct {
    uint8_t kind; /* sched_trigger_kind_t */
    uint8_t _pad[7];
    union {
        struct { int64_t period_ms; int64_t phase_ms; } every;
        sched_cron_t cron;
        struct { uint8_t hh, mm; } at;
        struct { uint8_t days_mask, hh, mm; } weekly; /* bit i = weekday i, 0=Sun */
        struct { uint8_t event; int32_t offset_s; } sun; /* ±12 h, compile-checked */
    } u;
} sched_trigger_t;

/* Window edge: SUN_EXPR (event + signed offset) or CLOCK (HH:MM). */
typedef enum { SCHED_EDGE_CLOCK, SCHED_EDGE_SUN } sched_edge_kind_t;

typedef struct {
    uint8_t  kind;     /* sched_edge_kind_t */
    uint8_t  event;    /* TIME_SYNC_SUNRISE/SUNSET when kind == SCHED_EDGE_SUN */
    uint8_t  hh, mm;   /* when kind == SCHED_EDGE_CLOCK */
    int32_t  offset_s; /* sun edges only */
} sched_edge_t;

typedef enum { SCHED_UNRESOLVED_SKIP = 0, SCHED_UNRESOLVED_RUN = 1 } sched_unresolved_t;

/* hint remembers whether the window was lowered from `day`/`night` so the
 * polar fallback follows design §Gates. */
typedef enum { SCHED_WIN_EXPLICIT = 0, SCHED_WIN_DAY = 1, SCHED_WIN_NIGHT = 2 } sched_window_hint_t;

typedef struct {
    sched_edge_t from, to;
    uint8_t      unresolved; /* sched_unresolved_t */
    uint8_t      hint;       /* sched_window_hint_t */
} sched_window_t;

typedef enum {
    SCHED_WINDOW_OPEN,
    SCHED_WINDOW_CLOSED,
    SCHED_WINDOW_UNRESOLVED, /* sun edge NOT_FOUND; gate maps via unresolved: */
} sched_window_state_t;

/* What to do when the job is still running at the next firing (JII standard
 * deployment-schedule enums; jobs are sequential on one task, so queue-one
 * defers at most one firing, never a backlog). */
typedef enum {
    SCHED_OVERLAP_SKIP = 0,      /* drop the new firing (default) */
    SCHED_OVERLAP_QUEUE_ONE = 1, /* allow exactly one pending firing */
    SCHED_OVERLAP_REJECT = 2,    /* drop it and count a failure */
} sched_overlap_t;

/* What to do about a slot missed while busy (late past grace). */
typedef enum {
    SCHED_MISSED_SKIP = 0,     /* count and drop (default) */
    SCHED_MISSED_RUN_ONCE = 1, /* one make-up run (the old catch_up_once) */
} sched_missed_t;

typedef struct sched_step {
    const sched_action_t *action; /* resolved at compile; run == NULL until T3 */
    uint16_t entry_start;         /* into the program's entry pool */
    uint8_t  entry_count;
    uint8_t  continue_on_error;   /* default false: a step failure aborts the job */
    uint8_t  _pad[2];
} sched_step_t;

typedef struct {
    uint16_t name_off;
    uint8_t  trigger_count;
    uint8_t  step_count;
    uint8_t  overlap;    /* sched_overlap_t */
    uint8_t  missed;     /* sched_missed_t */
    uint8_t  on_enter;   /* fire once when the gate opens; default true for gated jobs */
    uint8_t  has_window;
    uint8_t  _pad;
    sched_window_t  window;
    sched_trigger_t triggers[SCHED_SPEC_MAX_TRIGGERS];
    sched_step_t    steps[SCHED_SPEC_MAX_STEPS];
} sched_job_t;

typedef struct sched_program {
    /* Document header (JII idiom): schema is validated at compile; the rest
     * is provenance only — carried, logged, never acted on. */
    uint16_t id_off;                  /* urn:jii:schedule:… or SCHED_POOL_NONE */
    uint16_t version_off;             /* semver-ish string or NONE */
    uint16_t workbook_version_id_off; /* openJII workbook uuid or NONE */
    uint16_t name_off;
    uint16_t description_off;
    uint8_t  protocol_count;
    uint8_t  job_count;
    uint16_t entry_count;
    uint16_t pool_used;
    sched_protocol_t protocols[SCHED_SPEC_MAX_PROTOCOLS];
    sched_job_t      jobs[SCHED_SPEC_MAX_JOBS];
    sched_entry_t    entries[SCHED_SPEC_MAX_ENTRIES];
    char             pool[SCHED_SPEC_STRING_POOL];
} sched_program_t;

const char *sched_pool_str(const sched_program_t *p, uint16_t off); /* NULL when NONE */
const sched_job_t *sched_find_job(const sched_program_t *p, const char *name);

/* Node tree → program. Strict: unknown keys are errors. err is
 * "line:col: message", the same string on device logs, `schedule validate`,
 * the script_status failure detail and CI. */
esp_err_t sched_compile(const sched_node_t *root, sched_program_t *out,
                        char *err, size_t err_cap);
/* Parse + compile in one call; frees the arena before returning. */
esp_err_t sched_compile_text(const char *text, size_t len, sched_program_t *out,
                             char *err, size_t err_cap);

/* Duration estimate exposed for the runner's poll scheduling (90 % poll
 * start, broken-channel deadline): Σ(pulses/freq·1000 + 300) ms — 300 ms
 * per-segment configuration/light-sleep slack, the established field formula.
 * NOTE: the compiler's duration-vs-period rule
 * deliberately uses pulse time + deadline_margin without this overhead (see
 * sched_compile.c for why). */
int64_t sched_estimate_ms(const sched_protocol_t *proto);

/* ── cron ────────────────────────────────────────────────────────────── */

typedef struct {
    int year, month, day, hour, min, wday; /* wday 0=Sun; year/month/day for masks */
} sched_tm_like_t;

esp_err_t sched_cron_parse(const char *expr, sched_cron_t *out,
                           char *err, size_t err_cap);
bool sched_cron_matches(const sched_cron_t *c, const sched_tm_like_t *t);
/* Next fire strictly after after_local (LOCAL unix), bounded at 366 days;
 * ESP_ERR_NOT_FOUND beyond that. Operates on the linear local frame:
 * spring-forward times that never materialise on the wall are still frame
 * instants — the runner's poll skips them because its localised now jumps
 * past, and the fall-back double wall minute is de-duplicated by the due
 * model's forward-only dues + fired-minute latch. */
esp_err_t sched_cron_next(const sched_cron_t *c, int64_t after_local, int64_t *out_local);

/* ── window math ─────────────────────────────────────────────────────── */

sched_window_state_t sched_window_state(const sched_window_t *win, int64_t now_local);
/* Earliest instant > now_local at which the window opens; false when no edge
 * resolves within 2 days (polar) — the gate then stays on its unresolved
 * policy until the sun comes back. */
bool sched_window_next_open(const sched_window_t *win, int64_t now_local, int64_t *out_local);

/* ── due-time model (pure; injected clock) ───────────────────────────── */

/* LOCAL = localize(ctx, UTC). The runner supplies the timezone component's
 * offset here; host tests pass a fixed-offset stub. */
typedef int64_t (*sched_localize_fn)(void *ctx, int64_t utc_unix);

/* A forward wall correction can legitimately project an overdue armed instant
 * before monotonic zero, so -1 is not a safe sentinel in the monotonic domain. */
#define SCHED_DUE_NONE_US INT64_MIN

typedef struct {
    int64_t due_us[SCHED_SPEC_MAX_TRIGGERS]; /* armed monotonic µs; INT64_MIN=none */
    int64_t wall_due_local[SCHED_SPEC_MAX_TRIGGERS]; /* paired wall anchor used
                                             * only to resolve wall successors */
    int64_t fired_minute;  /* last local minute a cron trigger fired in (DST latch) */
    uint32_t skipped;      /* runnable slots missed past grace; a lower bound
                            * while skipped_saturated is 1 */
    uint32_t runs;         /* firings handed to the runner */
    uint8_t  boot_pending; /* boot trigger not yet consumed */
    uint8_t  gate_open;    /* last evaluated gate state (on_enter edge detect) */
    uint8_t  skipped_saturated; /* this job's stale backlog exceeded its
                            * per-poll walk budget; the rest was jumped */
} sched_due_job_t;

typedef struct {
    const sched_program_t *prog;
    sched_localize_fn      localize;
    void                  *localize_ctx;
    int64_t                anchor_wall_utc;
    int64_t                anchor_mono_us;
    int32_t                anchor_offset_s;
    sched_due_job_t        jobs[SCHED_SPEC_MAX_JOBS];
} sched_due_t;

/* Dual-clock runner API. Wall UTC is sampled only when anchoring/re-anchoring;
 * all armed dues, polling and waits use monotonic microseconds. The wall
 * projection advances from the anchor by monotonic elapsed time, so corrections
 * inside the runner's >2 s re-anchor threshold cannot stretch/compress an
 * `every` cadence. */
void sched_due_init_mono(sched_due_t *d, const sched_program_t *prog,
                         sched_localize_fn localize, void *ctx,
                         int64_t now_utc, int64_t now_mono_us);
uint32_t sched_due_poll_mono(sched_due_t *d, int64_t now_mono_us);
int64_t sched_due_next_mono(const sched_due_t *d, int64_t now_mono_us);
void sched_due_reanchor_mono(sched_due_t *d, int64_t now_utc,
                             int64_t now_mono_us);
/* Wall UTC projected from the last anchor and monotonic elapsed time. The
 * runner compares this with gettimeofday() for cumulative step/slew detection
 * and uses it to render a monotonic due in status output. */
int64_t sched_due_project_wall_utc(const sched_due_t *d, int64_t mono_us);

/* Compatibility seconds API for pure callers/tests: it maps UTC seconds onto
 * the same numeric monotonic timeline. Firmware uses the dual-clock API above.
 * Computes initial dues from now; boot triggers go pending (the runner calls
 * this after clock trust, so boot means first trusted time). No job fires as
 * a side effect of init, and the initial gate state is not an "entry". */
void sched_due_init(sched_due_t *d, const sched_program_t *prog,
                    sched_localize_fn localize, void *ctx, int64_t now_utc);

/* Advance the model to now_utc; returns the bitmask of jobs to fire now.
 * Late slots (older than grace: period for `every`, 600 s otherwise) are
 * counted in `skipped` and dropped (missed: skip) or fired once late
 * (missed: run-once). Slots that passed while the gate was closed advance
 * silently. A poll's stale-slot walk is budget-bounded; a jumped backlog
 * sets skipped_saturated and leaves skipped a lower bound.
 * overlap: queue-one/reject affect execution, which the runner
 * owns; the model only ever hands out one firing per poll per job. */
uint32_t sched_due_poll(sched_due_t *d, int64_t now_utc);

/* Soonest local unix > now at which poll() could produce a firing or a gate
 * entry; -1 when nothing can fire (e.g. dispatch-only program). */
int64_t sched_due_next(const sched_due_t *d, int64_t now_utc);

/* Clock-step re-anchor: call after the wall clock was set/corrected (the
 * runner detects the step by comparing wall vs monotonic elapsed), BEFORE the
 * next poll. Dues still ahead of the new now were anchored to the old,
 * larger timebase (backward correction) and are recomputed from the new local
 * now, so an every-job resumes within one period instead of waiting out a
 * stale due. Dues already at/past the new now are left alone so the next
 * poll applies the late-grace/missed accounting (a forward correction
 * surfaces as counted skipped slots or one make-up run, per `missed:`).
 * boot_pending, skipped/runs counters, the fired-minute latch and the gate
 * state are preserved: re-anchoring must not re-arm a consumed boot trigger,
 * erase statistics, or manufacture an on_enter edge. */
void sched_due_reanchor(sched_due_t *d, int64_t now_utc);

#ifdef __cplusplus
}
#endif

#endif /* AMBYTE_SCHED_SPEC_H */
