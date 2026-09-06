/*
 * sched_runner — task, lifecycle, timebase and job execution for the
 * declarative schedule. It owns the measurement task (see the epic
 * design: components/sched_spec compiles /littlefs/schedule.yaml or the
 * embedded default into a bounded sched_program_t; this task executes it).
 *
 * Timebase (plan §Timebase): sched_due arms monotonic microsecond instants;
 * wall UTC is sampled only at init/re-anchor and projected from monotonic
 * elapsed between them. Clock-step detection compares real wall with that
 * projection; a divergence over 2 s reprojects wall triggers before polling.
 * Thus small corrections/slews cannot stretch a fixed seconds-first cron
 * cadence, while calendar cron, solar events, and DST keep wall semantics. The
 * 60 s wait ceiling
 * bounds how long a step can go unnoticed.
 *
 * Lifecycle: an explicit state machine (STOPPED/STARTING/RUNNING/STOPPING)
 * guarded by the s_state_mux spinlock, separate from the task handle. start()
 * claims STARTING before compiling (a concurrent start gets INVALID_STATE —
 * no double task, no shared s_prog compile); the task transitions to STOPPED
 * under the same lock before exiting. stop()/dispatch() wake the task through
 * a STATIC, never-deleted semaphore (not a task notification), so there is no
 * task handle to race with exit; a wake that lands after the task exited is a
 * harmless stale token the next start() drains. The same semaphore gates a
 * newly created high-priority task until start() has published RUNNING or
 * STOPPING, so the child cannot outrun its creator. Dispatch is accepted only
 * in RUNNING, never silently discarded mid-stop. Status readers snapshot
 * under the lock and see "no program" while a compile is in flight.
 *
 * Overlap (skip/queue-one/reject) is execution semantics, owned here: the
 * single-task loop runs jobs synchronously, so the only firing that can find
 * a job mid-run is an on-demand dispatch. Scheduled busy slots are the due
 * model's late-grace/missed path, which hands out at most one firing per job
 * per poll — at most one deferred run by construction.
 *
 * Resident memory (T3 review): s_prog is ~15.7 KB of static BSS, s_due ~1.4 KB,
 * s_rt 512 B, the action pending table 4.8 KB — all static, not heap. The
 * parse arena is freed by sched_compile_text. The one big heap reservation is
 * ambit_trace's 64,536 B payload buffer, taken once at first start and NEVER
 * released (deliberate: one early contiguous block beats per-trace
 * fragmentation); a stop/start cycle costs only the task/TCB (~9 KB).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"

#include "ambit_trace.h"
#include "device_commands.h"
#include "device_config.h"
#include "sched_runner.h"
#include "sched_runner_priv.h"
#include "time_sync.h"
#include "timezone.h"

#define TAG "sched_runner"

#define SCHED_TASK_NAME "sched_runner"
#define SCHED_TASK_STACK 8192
/* Measurement + local storage runs above all application comms tasks so a
 * scheduled measurement fires promptly and its event-log write is prioritised
 * over MQTT publishing (esp-mqtt task = 5, sync_runner = 3). Kept WELL below
 * the network stack — LwIP TCP/IP (18) and Wi-Fi (~23) must stay higher or
 * the radio link breaks; those tasks are bursty/idle so they don't slow us.
 * (Rationale retained from the previous measurement runner.) */
#define SCHED_TASK_PRIORITY 10
/* Pin to APP_CPU (core 1). Wi-Fi (pinned core 0) and LwIP (prio 18) outrank
 * the measurement (prio 10), so an unpinned runner task that lands on core 0
 * gets preempted mid-UART-transaction → FSM/timing jitter. Pinning the
 * timing-sensitive measurement to core 1 keeps it off the radio's core; the
 * slack when it blocks is still reused by other tasks allowed on core 1.
 * (ESP32-S3 is dual-core; retained from the previous runner.) */
#define SCHED_TASK_CORE 1

/* The schedule lives on power-loss-safe internal littlefs; no SD appears
 * anywhere in this path (design decision 7). */
#define SCHED_PATH "/littlefs/schedule.yaml"

/* On-demand dispatches (CLI `schedule run`, MQTT schedule_run). Four is
 * plenty: an operator pokes one job at a time; a full queue means someone is
 * hammering and ESP_ERR_NO_MEM is the honest answer. */
#define DISPATCH_QUEUE_LEN 4

/* The wait ceiling is the clock-step check cadence: an RTC correction is
 * noticed within one sleep even when nothing is due for hours. */
#define LOOP_MAX_WAIT_MS 60000
/* Monotonic vs wall divergence beyond this means the RTC was set/corrected. */
#define CLOCK_STEP_TOLERANCE_S 2

/* Embedded default schedule, generated at configure time (see CMakeLists). */
#include "default_yaml_embed.h"

/* ── lifecycle state (guarded by s_state_mux) ──────────────────────────── */

/* The envelope provenance port (domain) and the compiler cap must agree, or
 * a fully-stamped header would silently lose macros on the wire. */
_Static_assert(SCHEDULE_PROVENANCE_MAX_MACROS == SCHED_SPEC_MAX_MACROS,
               "envelope provenance port must hold every compiled macro");

/* Spinlock critical sections are never held across a blocking call — only
 * state/flag reads+writes and short struct copies. The one exception is each
 * first xSemaphoreCreateBinaryStatic: it only initializes caller-owned
 * storage (no allocation or wait) and runs under this lock so concurrent first
 * starts cannot initialize the same StaticSemaphore_t twice. Lock order when
 * nested: s_state_mux → s_dispatch_mux → s_stats_mux. */
static portMUX_TYPE s_state_mux = portMUX_INITIALIZER_UNLOCKED;
static sched_lifecycle_t s_lifecycle = SCHED_LIFECYCLE_INITIALIZER;

/* Stop flag polled by the loop and by every action between UART transactions.
 * Written only under s_state_mux; polled lock-free (a byte write is atomic on
 * Xtensa and the poll is repeated every ≤100 ms, no torn-read hazard). */
static volatile bool s_should_stop = false;

/* Wake signal for the loop's wait (stop or a queued dispatch). STATIC and
 * never deleted, so stop()/dispatch() can give it outside the state lock
 * without racing the task's exit — a stale give is just an extra wake the
 * next start() drains. */
static SemaphoreHandle_t s_wake_sem = NULL;
static StaticSemaphore_t s_wake_sem_storage;

/* Signaled by the task when it has fully exited, so stop() can wait and a
 * later start() cannot overlap two runner tasks. Created lazily, BEFORE the
 * STARTING claim, so a stop during STARTING can wait on it. stop() re-gives
 * after a successful take: the exit signal stays latched (and concurrent
 * stoppers pass) until the next start() drains it. */
static SemaphoreHandle_t s_done_sem = NULL;
static StaticSemaphore_t s_done_sem_storage;

static sched_program_t s_prog;       /* the single static program (plan) */
static bool            s_prog_valid = false; /* under s_state_mux */
static sched_due_t     s_due;        /* runner task only, except the snapshot */
static sched_source_t  s_source = { SCHED_SOURCE_NONE, "", "" }; /* s_state_mux */

/* Runner-owned execution counters under s_stats_mux; the due model's skipped
 * reaches status through the snapshot below. */
typedef struct {
    uint32_t runs, failures, fail_streak;
    int64_t  last_run_epoch;
    uint32_t last_duration_ms;
} job_rt_t;
static portMUX_TYPE s_stats_mux = portMUX_INITIALIZER_UNLOCKED;
static job_rt_t s_rt[SCHED_SPEC_MAX_JOBS];

/* Status snapshot of the due model, published by the runner task after init
 * and every poll; the CLI reads s_due only through this copy (a live read
 * could tear against poll's 64-bit due updates). */
typedef struct {
    int64_t  next_due_utc; /* wall projection of armed monotonic due */
    uint32_t skipped;
    uint8_t  boot_pending;
    uint8_t  skipped_saturated;
} job_snap_t;
static job_snap_t s_snap[SCHED_SPEC_MAX_JOBS]; /* under s_state_mux */

/* Dispatch queue + the queue-one deferred latch (both under s_dispatch_mux;
 * touched by the CLI and MQTT tasks). */
static portMUX_TYPE s_dispatch_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t s_dispatch_q[DISPATCH_QUEUE_LEN];
static int     s_dispatch_head = 0, s_dispatch_len = 0;
static int     s_running_job = -1;   /* runner task writes; dispatch reads */
static uint32_t s_deferred = 0;      /* queue-one latch bits */

/* Action context (sched_runner_actions.c reads it per step). */
sched_runner_act_ctx_t s_act_ctx;
static bool    s_loc_warned = false; /* one WARN per boot, not per restart */

/* ── helpers ─────────────────────────────────────────────────────────── */

static int64_t wall_now_utc(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL); /* RTC-backed system time; the RTC holds UTC */
    return (int64_t)tv.tv_sec;
}

/* The due model's localize: the timezone component owns the DST-correct
 * offset and pushes it into time_sync as a side effect, so the sun math in
 * sched_due stays on the same frame. */
static int64_t runner_localize(void *ctx, int64_t utc)
{
    (void)ctx;
    return timezone_localize(utc);
}

static void sha256_hex(const uint8_t *data, size_t len, char out[65])
{
    uint8_t d[32];
    mbedtls_sha256(data, len, d, 0 /* SHA-256, not 224 */);
    for (int i = 0; i < 32; i++) sprintf(out + 2 * i, "%02x", d[i]);
    out[64] = '\0';
}

/* Read a littlefs schedule file into a heap buffer. ESP_ERR_NOT_FOUND when
 * absent, ESP_ERR_INVALID_SIZE when over the parser's file cap. */
static esp_err_t read_schedule_file(const char *path, char **out_buf, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) return ESP_ERR_NOT_FOUND;
    char *buf = malloc(SCHED_YAML_MAX_FILE_BYTES + 1);
    if (buf == NULL) { fclose(f); return ESP_ERR_NO_MEM; }
    size_t len = fread(buf, 1, SCHED_YAML_MAX_FILE_BYTES + 1, f);
    fclose(f);
    if (len > SCHED_YAML_MAX_FILE_BYTES) {
        free(buf);
        return ESP_ERR_INVALID_SIZE;
    }
    *out_buf = buf;
    *out_len = len;
    return ESP_OK;
}

static const char *source_kind_str(sched_source_kind_t k)
{
    switch (k) {
    case SCHED_SOURCE_INSTALLED:         return "INSTALLED";
    case SCHED_SOURCE_EMBEDDED_DEFAULT:  return "EMBEDDED_DEFAULT";
    case SCHED_SOURCE_EMBEDDED_FALLBACK: return "EMBEDDED_FALLBACK";
    default:                             return "NONE";
    }
}

/* Compile the embedded default into s_prog. A compile error here is a build
 * bug — the file is fixed at configure time and host-tested — so assert.
 * Caller holds the STARTING claim: s_prog is exclusively ours here. */
static void compile_embedded_or_die(void)
{
    char err[128];
    if (sched_compile_text(default_yaml_start, default_yaml_size, &s_prog,
                           err, sizeof(err)) != ESP_OK) {
        ESP_LOGE(TAG, "embedded default.yaml does not compile (%s) — build bug", err);
        abort();
    }
}

static void apply_location_from_config(void)
{
    double lat, lon;
    if (device_config_get_location(&lat, &lon, s_act_ctx.deployment,
                                   sizeof(s_act_ctx.deployment)) == ESP_OK) {
        int tz;
        time_sync_get_location(NULL, NULL, &tz); /* keep the configured tz */
        time_sync_set_location(lat, lon, tz);
        ESP_LOGI(TAG, "location from device_config: lat=%.5f lon=%.5f", lat, lon);
    } else if (!s_loc_warned) {
        /* One WARN per boot: sun triggers
         * and windows fall back to time_sync's compiled NL default. */
        ESP_LOGW(TAG, "no lat/lon in device_config — sun triggers use the "
                      "compiled default (52.173N 5.819E)");
        s_loc_warned = true;
    }
}

/* Copy the CLI-visible due-model state out of s_due. Runner task only. */
static void publish_status_snapshot(void)
{
    job_snap_t tmp[SCHED_SPEC_MAX_JOBS];
    for (int j = 0; j < s_prog.job_count; j++) {
        int64_t best = SCHED_DUE_NONE_US;
        for (int t = 0; t < s_prog.jobs[j].trigger_count; t++) {
            int64_t dd = s_due.jobs[j].due_us[t];
            if (dd != SCHED_DUE_NONE_US &&
                (best == SCHED_DUE_NONE_US || dd < best)) best = dd;
        }
        tmp[j].next_due_utc =
            best == SCHED_DUE_NONE_US
                ? -1
                : sched_due_project_wall_utc(&s_due, best);
        tmp[j].skipped = s_due.jobs[j].skipped;
        tmp[j].boot_pending = s_due.jobs[j].boot_pending;
        tmp[j].skipped_saturated = s_due.jobs[j].skipped_saturated;
    }
    taskENTER_CRITICAL(&s_state_mux);
    memcpy(s_snap, tmp, sizeof(tmp));
    taskEXIT_CRITICAL(&s_state_mux);
}

/* ── job execution ───────────────────────────────────────────────────── */

static void run_job(int j);

/* overlap=queue-one latch: the firing that arrived mid-run gets exactly one
 * deferred execution, never a backlog. */
static void maybe_run_deferred(int j)
{
    bool go;
    taskENTER_CRITICAL(&s_dispatch_mux);
    go = (s_deferred & (1u << j)) != 0;
    s_deferred &= ~(1u << j);
    taskEXIT_CRITICAL(&s_dispatch_mux);
    if (go && !s_should_stop) {
        ESP_LOGI(TAG, "%s: running the one queued overlap firing", sched_pool_str(&s_prog, s_prog.jobs[j].name_off));
        run_job(j);
    }
}

static void run_job(int j)
{
    const sched_job_t *job = &s_prog.jobs[j];
    const char *name = sched_pool_str(&s_prog, job->name_off);
    if (s_should_stop) return;

    taskENTER_CRITICAL(&s_dispatch_mux);
    s_running_job = j;
    taskEXIT_CRITICAL(&s_dispatch_mux);
    /* Snapshot the counters the $job.* placeholders resolve from. */
    s_act_ctx.job_idx     = j;
    s_act_ctx.job_name    = name;
    s_act_ctx.runs        = s_rt[j].runs;
    s_act_ctx.failures    = s_rt[j].failures;
    s_act_ctx.skipped     = s_due.jobs[j].skipped;
    s_act_ctx.skipped_saturated = s_due.jobs[j].skipped_saturated != 0;
    s_act_ctx.fail_streak = s_rt[j].fail_streak;
    s_act_ctx.fail_detail[0] = '\0';

    const int64_t t0_us = esp_timer_get_time();
    bool failed = false;
    const char *fail_step = NULL;
    esp_err_t fail_err = ESP_OK;
    char fail_detail[sizeof(s_act_ctx.fail_detail)] = "";
    for (int s = 0; s < job->step_count && !s_should_stop; s++) {
        const sched_step_t *step = &job->steps[s];
        esp_err_t err = step->action->run(&s_act_ctx, step, &s_prog);
        if (err != ESP_OK) {
            if (s_should_stop) break; /* a stop abort is not a job failure */
            if (!failed) {
                fail_step = step->action->name;
                fail_err  = err;
                snprintf(fail_detail, sizeof(fail_detail), "%s", s_act_ctx.fail_detail);
            }
            failed = true;
            if (!step->continue_on_error) break;
        }
    }
    const uint32_t dur_ms = (uint32_t)((esp_timer_get_time() - t0_us) / 1000);
    const int64_t  epoch  = wall_now_utc();

    sched_fail_log_t decision = SCHED_FAIL_LOG_NONE;
    uint32_t streak = 0;
    if (!s_should_stop) {
        taskENTER_CRITICAL(&s_stats_mux);
        s_rt[j].last_duration_ms = dur_ms;
        s_rt[j].last_run_epoch   = epoch;
        if (failed) {
            s_rt[j].failures++;
            s_rt[j].fail_streak++;
            streak   = s_rt[j].fail_streak;
            decision = sched_fail_log_decision(true, streak);
        } else {
            s_rt[j].runs++;
            streak   = s_rt[j].fail_streak;
            decision = sched_fail_log_decision(false, streak);
            s_rt[j].fail_streak = 0;
        }
        taskEXIT_CRITICAL(&s_stats_mux);
    }
    /* THE single failure-log path (T3 review blocker 4): actions write
     * fail_detail and stay silent on routine failures, so a persistently
     * failing job logs the onset, every 300th, and the recovery — never one
     * line per firing (the 536 K-line flood this policy exists to prevent). */
    if (decision == SCHED_FAIL_LOG_FAILURE) {
        ESP_LOGW(TAG, "%s: step %s failed: %s%s%s (streak %lu, next log at %lu)",
                 name, fail_step, esp_err_to_name(fail_err),
                 fail_detail[0] ? ": " : "", fail_detail,
                 (unsigned long)streak,
                 (unsigned long)(streak / SCHED_FAIL_STREAK_LOG_EVERY + 1) *
                     SCHED_FAIL_STREAK_LOG_EVERY);
    } else if (decision == SCHED_FAIL_LOG_RECOVERY) {
        ESP_LOGI(TAG, "%s: recovered after %lu consecutive failures",
                 name, (unsigned long)streak);
    }
    taskENTER_CRITICAL(&s_dispatch_mux);
    s_running_job = -1;
    taskEXIT_CRITICAL(&s_dispatch_mux);
    maybe_run_deferred(j);
}

/* ── dispatch (other tasks → runner task) ────────────────────────────── */

esp_err_t sched_runner_dispatch(const char *job_name)
{
    /* Accepted only in RUNNING: a dispatch while stopped/stopping is rejected
     * with INVALID_STATE, never queued-then-silently-discarded. s_prog is
     * stable while RUNNING (compiles happen only under the STARTING claim). */
    taskENTER_CRITICAL(&s_state_mux);
    if (s_lifecycle.state != SCHED_LIFE_RUNNING) {
        taskEXIT_CRITICAL(&s_state_mux);
        return ESP_ERR_INVALID_STATE;
    }
    const sched_job_t *job = sched_find_job(&s_prog, job_name);
    if (job == NULL) {
        taskEXIT_CRITICAL(&s_state_mux);
        return ESP_ERR_NOT_FOUND;
    }
    int j = (int)(job - s_prog.jobs);

    esp_err_t ret = ESP_OK;
    enum { D_NONE, D_DEFERRED, D_REJECTED, D_SKIPPED } outcome = D_NONE;
    taskENTER_CRITICAL(&s_dispatch_mux);
    bool queued = false;
    for (int i = 0; i < s_dispatch_len; i++) {
        if (s_dispatch_q[(s_dispatch_head + i) % DISPATCH_QUEUE_LEN] == j) queued = true;
    }
    if (s_running_job == j || queued) {
        /* The firing found the job busy; the compiled overlap policy decides. */
        switch (job->overlap) {
        case SCHED_OVERLAP_QUEUE_ONE:
            if (s_running_job == j) {
                s_deferred |= (1u << j); /* at most one, by latch construction */
            } /* already queued: the queued one IS the one deferred firing */
            outcome = D_DEFERRED;
            break;
        case SCHED_OVERLAP_REJECT:
            taskENTER_CRITICAL(&s_stats_mux);
            s_rt[j].failures++;
            taskEXIT_CRITICAL(&s_stats_mux);
            outcome = D_REJECTED;
            break;
        case SCHED_OVERLAP_SKIP:
        default:
            outcome = D_SKIPPED;
            break;
        }
    } else if (s_dispatch_len >= DISPATCH_QUEUE_LEN) {
        ret = ESP_ERR_NO_MEM;
    } else {
        s_dispatch_q[(s_dispatch_head + s_dispatch_len) % DISPATCH_QUEUE_LEN] = (uint8_t)j;
        s_dispatch_len++;
    }
    taskEXIT_CRITICAL(&s_dispatch_mux);
    taskEXIT_CRITICAL(&s_state_mux);

    /* Wake outside the lock: s_wake_sem is static and never deleted, so this
     * give cannot race the task's exit — a stale give is an extra wake. */
    if (ret == ESP_OK && outcome == D_NONE) xSemaphoreGive(s_wake_sem);

    switch (outcome) {
    case D_DEFERRED: ESP_LOGI(TAG, "%s: dispatch while busy — one firing deferred (queue-one)", job_name); break;
    case D_REJECTED: ESP_LOGW(TAG, "%s: dispatch while busy — rejected, counted as failure", job_name); break;
    case D_SKIPPED:  ESP_LOGI(TAG, "%s: dispatch while busy — dropped (overlap skip)", job_name); break;
    default: break;
    }
    return ret;
}

static void drain_dispatch(void)
{
    for (;;) {
        taskENTER_CRITICAL(&s_dispatch_mux);
        if (s_dispatch_len == 0) {
            taskEXIT_CRITICAL(&s_dispatch_mux);
            return;
        }
        int j = s_dispatch_q[s_dispatch_head];
        s_dispatch_head = (s_dispatch_head + 1) % DISPATCH_QUEUE_LEN;
        s_dispatch_len--;
        taskEXIT_CRITICAL(&s_dispatch_mux);
        if (!s_should_stop) run_job(j);
    }
}

/* ── the task ────────────────────────────────────────────────────────── */

static void sched_runner_task(void *arg)
{
    const uint32_t generation = (uint32_t)(uintptr_t)arg;

    /* xTaskCreatePinnedToCore may immediately schedule this higher-priority
     * task on the other core. Wait until start() has published the lifecycle
     * state and stop flag; without this gate a restart could observe the
     * previous run's true stop flag or outrun a concurrent stop-in-STARTING. */
    (void)xSemaphoreTake(s_wake_sem, portMAX_DELAY);

    /* Due-model init happens on the task, after start(): boot triggers go
     * pending here, and start happens after clock trust (same call-site
     * call-site contract), so boot means first trusted time. */
    const int64_t init_mono_us = esp_timer_get_time();
    sched_due_init_mono(&s_due, &s_prog, runner_localize, NULL,
                        wall_now_utc(), init_mono_us);
    publish_status_snapshot();

    const char *name = sched_pool_str(&s_prog, s_prog.name_off);
    ESP_LOGI(TAG, "running '%s' (%u jobs, source %s, sha256 %.12s…)",
             name ? name : "(unnamed)", (unsigned)s_prog.job_count,
             source_kind_str(s_source.kind), s_source.sha256);

    /* The sunrise/sunset line is
     * the first thing a field tech greps for. */
    {
        int64_t L = timezone_localize(wall_now_utc());
        char sr[8] = "--:--", ss[8] = "--:--";
        int64_t u;
        int h, m;
        if (time_sync_sun_on_date(L, TIME_SYNC_SUNRISE, &u) == ESP_OK) {
            time_sync_localtime(u, NULL, NULL, NULL, &h, &m, NULL, NULL);
            snprintf(sr, sizeof(sr), "%02d:%02d", h, m);
        }
        if (time_sync_sun_on_date(L, TIME_SYNC_SUNSET, &u) == ESP_OK) {
            time_sync_localtime(u, NULL, NULL, NULL, &h, &m, NULL, NULL);
            snprintf(ss, sizeof(ss), "%02d:%02d", h, m);
        }
        ESP_LOGI(TAG, "schedule started; sunrise=%s sunset=%s", sr, ss);
    }

    for (;;) {
        if (s_should_stop) break;

        /* Compare real wall UTC with the wall projected from the last
         * monotonic anchor. Unlike per-iteration deltas, this catches a slow
         * correction once cumulative divergence exceeds the 2 s threshold. */
        const int64_t now  = wall_now_utc();
        const int64_t mono = esp_timer_get_time();
        const int64_t wall_error =
            now - sched_due_project_wall_utc(&s_due, mono);
        if (wall_error > CLOCK_STEP_TOLERANCE_S ||
            wall_error < -CLOCK_STEP_TOLERANCE_S) {
            ESP_LOGW(TAG, "clock stepped %lld s — re-anchoring wall triggers",
                     (long long)wall_error);
            sched_due_reanchor_mono(&s_due, now, mono);
        }

        uint32_t mask = sched_due_poll_mono(&s_due, mono);
        publish_status_snapshot();
        for (int j = 0; j < s_prog.job_count; j++) {
            if (mask & (1u << j)) run_job(j);
        }
        drain_dispatch();
        if (s_should_stop) break;

        /* Armed dues and this wait are both monotonic. The 60 s ceiling exists
         * only so clock corrections are detected even when nothing is due. */
        const int64_t now2_us = esp_timer_get_time();
        int64_t wait_ms = LOOP_MAX_WAIT_MS;
        const int64_t next_us = sched_due_next_mono(&s_due, now2_us);
        if (next_us >= 0) {
            int64_t d = (next_us - now2_us) / 1000;
            if (d < 10) d = 10; /* overdue → spin once, don't busy-wait on 0 */
            if (d < wait_ms) wait_ms = d;
        }
        const int64_t deadline_us = esp_timer_get_time() + wait_ms * 1000;
        const int64_t remain_ms = (deadline_us - esp_timer_get_time()) / 1000;
        /* Woken early by stop() or dispatch() giving s_wake_sem. */
        if (remain_ms > 0) {
            (void)xSemaphoreTake(s_wake_sem, pdMS_TO_TICKS((uint32_t)remain_ms));
        }
    }

    /* Stack high-water at exit: the number that trims SCHED_TASK_STACK after
     * the hardware soak (acceptance criterion 4). */
    ESP_LOGI(TAG, "stopped; stack high-water mark %u bytes of %u",
             (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)),
             (unsigned)SCHED_TASK_STACK);
    /* Publish STOPPED and this run's completion generation in one lock-held
     * transition, then wake waiters. A new start may claim before the give, but
     * its stopper compares generations and discards that old wake; successful
     * stop for this generation can never observe STOPPING afterwards. */
    taskENTER_CRITICAL(&s_state_mux);
    (void)sched_lifecycle_complete(&s_lifecycle, generation);
    taskEXIT_CRITICAL(&s_state_mux);
    if (s_done_sem != NULL) xSemaphoreGive(s_done_sem);
    vTaskDelete(NULL);
}

/* ── lifecycle ───────────────────────────────────────────────────────── */

esp_err_t sched_runner_start(void)
{
    /* Create both static semaphores and claim STARTING in one section: only
     * one first caller can initialize their shared backing objects, only one
     * caller can compile into s_prog, and a stop that observes STARTING is
     * guaranteed to have a valid done semaphore to wait on. */
    taskENTER_CRITICAL(&s_state_mux);
    uint32_t generation = 0;
    if (s_lifecycle.state != SCHED_LIFE_STOPPED) {
        taskEXIT_CRITICAL(&s_state_mux);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_done_sem == NULL) {
        s_done_sem = xSemaphoreCreateBinaryStatic(&s_done_sem_storage);
        if (s_done_sem == NULL) {
            taskEXIT_CRITICAL(&s_state_mux);
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_wake_sem == NULL) {
        s_wake_sem = xSemaphoreCreateBinaryStatic(&s_wake_sem_storage);
        if (s_wake_sem == NULL) {
            taskEXIT_CRITICAL(&s_state_mux);
            return ESP_ERR_NO_MEM;
        }
    }
    (void)sched_lifecycle_begin_start(&s_lifecycle, &generation);
    s_should_stop    = false; /* clear the previous run before task creation */
    s_prog_valid     = false; /* status reads see "no program" during compile */
    taskEXIT_CRITICAL(&s_state_mux);

    /* Drain stale gives from a previous run so stop() waits on the NEW task's
     * exit and the new task's first wait doesn't consume an old wake. */
    (void)xSemaphoreTake(s_done_sem, 0);
    (void)xSemaphoreTake(s_wake_sem, 0);

    const size_t heap_before = esp_get_free_heap_size();

    /* Bind the action run functions once per boot; the table rows stay bound
     * across runner restarts (suspend/resume recompiles but the catalog is
     * firmware-fixed). */
    sched_runner_bind_actions();

    /* 1. Compile the installed schedule, else the embedded default. s_source
     * is built on the stack and published under the lock — status readers
     * never see a half-written struct. */
    sched_source_t src;
    char *buf = NULL;
    size_t len = 0;
    esp_err_t r = read_schedule_file(SCHED_PATH, &buf, &len);
    if (r == ESP_ERR_NOT_FOUND) {
        compile_embedded_or_die();
        src.kind = SCHED_SOURCE_EMBEDDED_DEFAULT;
        src.reason[0] = '\0';
        sha256_hex((const uint8_t *)default_yaml_start, default_yaml_size, src.sha256);
        ESP_LOGI(TAG, "no %s — running the embedded default", SCHED_PATH);
    } else if (r != ESP_OK) {
        ESP_LOGE(TAG, "%s unreadable (%s) — running the embedded default",
                 SCHED_PATH, esp_err_to_name(r));
        compile_embedded_or_die();
        src.kind = SCHED_SOURCE_EMBEDDED_FALLBACK;
        snprintf(src.reason, sizeof(src.reason), "read: %s", esp_err_to_name(r));
        sha256_hex((const uint8_t *)default_yaml_start, default_yaml_size, src.sha256);
    } else {
        char cerr[128];
        if (sched_compile_text(buf, len, &s_prog, cerr, sizeof(cerr)) == ESP_OK) {
            src.kind = SCHED_SOURCE_INSTALLED;
            src.reason[0] = '\0';
            sha256_hex((const uint8_t *)buf, len, src.sha256);
        } else {
            /* The line-numbered reason is the field diagnostic; keep it for
             * `schedule status` and run the known-good default. */
            ESP_LOGE(TAG, "%s: %s — running the embedded default", SCHED_PATH, cerr);
            compile_embedded_or_die();
            src.kind = SCHED_SOURCE_EMBEDDED_FALLBACK;
            snprintf(src.reason, sizeof(src.reason), "%s", cerr);
            sha256_hex((const uint8_t *)default_yaml_start, default_yaml_size, src.sha256);
        }
        free(buf);
    }
    /* The document header is provenance (JII idiom): carried, logged at
     * start, shown by `schedule status`, never acted on. s_prog is exclusively
     * ours until the publish below (s_prog_valid false ⇒ status readers bail),
     * so these pool reads need no lock. */
    ESP_LOGI(TAG, "schedule id=%s version=%s workbook=%s",
             sched_pool_str(&s_prog, s_prog.id_off) ?: "-",
             sched_pool_str(&s_prog, s_prog.version_off) ?: "-",
             sched_pool_str(&s_prog, s_prog.workbook_version_id_off) ?: "-");

    /* 2. Reserve the shared 64,536 B trace/fallback buffer while the heap is
     * contiguous (the shared payload buffer is deliberately reserved early;
     * ambit_ota and CLI raw runs share the buffer). Taken ONCE and never
     * released — stop/start cycles do not pay it again. */
    if (ambit_trace_reserve() != ESP_OK) {
        ESP_LOGW(TAG, "ambit trace buffer reserve (%uB) failed; will retry per-run",
                 (unsigned)AMBIT_RUN_BUFFER_CAP);
    }

    /* 3. Location from NVS (read-only; the writers arrive with T4). */
    apply_location_from_config();

    struct timeval tv;
    gettimeofday(&tv, NULL);
    s_act_ctx.boot_epoch = (int64_t)tv.tv_sec;

    /* 4. Spawn the task. No SD check: the embedded default covers a blank
     * unit and the event log self-gates on persistence availability. */
    taskENTER_CRITICAL(&s_stats_mux);
    memset(s_rt, 0, sizeof(s_rt));
    taskEXIT_CRITICAL(&s_stats_mux);
    taskENTER_CRITICAL(&s_dispatch_mux);
    s_running_job = -1;
    s_dispatch_head = s_dispatch_len = 0;
    s_deferred = 0;
    taskEXIT_CRITICAL(&s_dispatch_mux);

    /* Publish in ONE section: status readers saw "no program" until here, so
     * the snapshot/source they now observe is fully initialised. The task
     * overwrites the snapshot with real dues right after its init. */
    taskENTER_CRITICAL(&s_state_mux);
    for (int j = 0; j < SCHED_SPEC_MAX_JOBS; j++) s_snap[j].next_due_utc = -1;
    s_source     = src;
    s_prog_valid = true;
    taskEXIT_CRITICAL(&s_state_mux);

    TaskHandle_t h = NULL;
    BaseType_t created = xTaskCreatePinnedToCore(
        sched_runner_task, SCHED_TASK_NAME, SCHED_TASK_STACK,
        (void *)(uintptr_t)generation,
        SCHED_TASK_PRIORITY, &h, SCHED_TASK_CORE);

    taskENTER_CRITICAL(&s_state_mux);
    if (created != pdPASS) {
        (void)sched_lifecycle_complete(&s_lifecycle, generation);
        taskEXIT_CRITICAL(&s_state_mux);
        xSemaphoreGive(s_done_sem);
        return ESP_ERR_NO_MEM;
    }
    bool stop_requested = false;
    (void)sched_lifecycle_publish_start(&s_lifecycle, generation,
                                        &stop_requested);
    s_should_stop = stop_requested; /* a stop that landed during STARTING */
    taskEXIT_CRITICAL(&s_state_mux);
    /* Release the task's startup gate only after state + stop are published. */
    xSemaphoreGive(s_wake_sem);

    ESP_LOGI(TAG, "runner started (heap %u → %u free)",
             (unsigned)heap_before, (unsigned)esp_get_free_heap_size());
    return ESP_OK;
}

esp_err_t sched_runner_stop(uint32_t wait_ms)
{
    taskENTER_CRITICAL(&s_state_mux);
    const sched_lifecycle_state_t st = s_lifecycle.state;
    const uint32_t generation = sched_lifecycle_request_stop(&s_lifecycle);
    if (st == SCHED_LIFE_RUNNING) {
        s_should_stop = true;
    } else if (st == SCHED_LIFE_STARTING) {
        /* The task does not exist yet; start() publishes should_stop and
         * wakes the task at the end of its spawn, and the task exits at its
         * first loop check. */
        s_should_stop    = true;
    }
    taskEXIT_CRITICAL(&s_state_mux);

    if (generation == 0) return ESP_OK; /* nothing running */
    if (st == SCHED_LIFE_RUNNING) xSemaphoreGive(s_wake_sem); /* outside lock */

    /* Actions poll s_should_stop between UART transactions and inside poll
     * loops, so a stop normally lands within one poll interval (500 ms) plus
     * one transaction. A stop during a 30 s fetch returns TIMEOUT, as with
     * the previous runner, and the task exits when the fetch returns. */
    const TickType_t wait_ticks = pdMS_TO_TICKS(wait_ms);
    const TickType_t started = xTaskGetTickCount();
    for (;;) {
        taskENTER_CRITICAL(&s_state_mux);
        const bool complete =
            sched_lifecycle_generation_complete(&s_lifecycle, generation);
        taskEXIT_CRITICAL(&s_state_mux);
        if (complete) {
            /* Keep the hint latched for concurrent stoppers. start() drains it;
             * a late token is harmless because every waiter checks generation. */
            xSemaphoreGive(s_done_sem);
            return ESP_OK;
        }

        const TickType_t elapsed = xTaskGetTickCount() - started;
        if (elapsed >= wait_ticks ||
            xSemaphoreTake(s_done_sem, wait_ticks - elapsed) != pdTRUE) {
            /* Close the boundary race: completion may have published between
             * the last check and the timeout return. */
            taskENTER_CRITICAL(&s_state_mux);
            const bool completed_at_timeout =
                sched_lifecycle_generation_complete(&s_lifecycle, generation);
            taskEXIT_CRITICAL(&s_state_mux);
            if (completed_at_timeout) {
                xSemaphoreGive(s_done_sem);
                return ESP_OK;
            }
            ESP_LOGW(TAG, "stop: task still busy after %u ms — will exit later",
                     (unsigned)wait_ms);
            return ESP_ERR_TIMEOUT;
        }
        /* A stale completion token from an older generation is discarded and
         * the remaining timeout is spent waiting for our generation. */
    }
}

bool sched_runner_is_running(void)
{
    taskENTER_CRITICAL(&s_state_mux);
    bool running = (s_lifecycle.state != SCHED_LIFE_STOPPED);
    taskEXIT_CRITICAL(&s_state_mux);
    return running;
}

bool sched_runner_should_stop(void)
{
    return s_should_stop;
}

/* ── status queries (CLI) ────────────────────────────────────────────── */

void sched_runner_source(sched_source_t *out)
{
    if (out == NULL) return;
    taskENTER_CRITICAL(&s_state_mux);
    *out = s_source;
    taskEXIT_CRITICAL(&s_state_mux);
}

esp_err_t sched_runner_stats(const char *job_name, sched_job_stats_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_STATE;
    taskENTER_CRITICAL(&s_state_mux);
    if (!s_prog_valid) {
        taskEXIT_CRITICAL(&s_state_mux);
        return ESP_ERR_INVALID_STATE;
    }
    const sched_job_t *job = sched_find_job(&s_prog, job_name);
    if (job == NULL) {
        taskEXIT_CRITICAL(&s_state_mux);
        return ESP_ERR_NOT_FOUND;
    }
    int j = (int)(job - s_prog.jobs);
    taskENTER_CRITICAL(&s_stats_mux);
    out->runs             = s_rt[j].runs;
    out->failures         = s_rt[j].failures;
    out->fail_streak      = s_rt[j].fail_streak;
    out->last_run_epoch   = s_rt[j].last_run_epoch;
    out->last_duration_ms = s_rt[j].last_duration_ms;
    taskEXIT_CRITICAL(&s_stats_mux);
    out->skipped = s_snap[j].skipped;
    out->skipped_saturated = s_snap[j].skipped_saturated != 0;
    taskEXIT_CRITICAL(&s_state_mux);
    return ESP_OK;
}

esp_err_t sched_runner_header(sched_header_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_STATE;
    esp_err_t ret = ESP_OK;
    taskENTER_CRITICAL(&s_state_mux);
    if (!s_prog_valid) {
        ret = ESP_ERR_INVALID_STATE;
    } else {
        const char *id   = sched_pool_str(&s_prog, s_prog.id_off);
        const char *ver  = sched_pool_str(&s_prog, s_prog.version_off);
        const char *wb   = sched_pool_str(&s_prog, s_prog.workbook_version_id_off);
        const char *name = sched_pool_str(&s_prog, s_prog.name_off);
        snprintf(out->id, sizeof(out->id), "%s", id ? id : "-");
        snprintf(out->version, sizeof(out->version), "%s", ver ? ver : "-");
        snprintf(out->workbook, sizeof(out->workbook), "%s", wb ? wb : "-");
        snprintf(out->name, sizeof(out->name), "%s", name ? name : "-");
        out->has_workbook = wb != NULL;
        /* Bounded by SCHED_SPEC_MAX_MACROS (8 × ~140 B): s_state_mux is a
         * spinlock, so the whole snapshot copy must stay small — the fixed
         * field caps in sched_header_t are what guarantee that. */
        out->macro_count = s_prog.macro_count;
        for (int i = 0; i < s_prog.macro_count; i++) {
            const char *mid  = sched_pool_str(&s_prog, s_prog.macros[i].id_off);
            const char *mnam = sched_pool_str(&s_prog, s_prog.macros[i].name_off);
            const char *mfn  = sched_pool_str(&s_prog, s_prog.macros[i].filename_off);
            snprintf(out->macros[i].id, sizeof(out->macros[i].id), "%s", mid ? mid : "");
            snprintf(out->macros[i].name, sizeof(out->macros[i].name), "%s", mnam ? mnam : "");
            snprintf(out->macros[i].filename, sizeof(out->macros[i].filename), "%s", mfn ? mfn : "");
        }
    }
    taskEXIT_CRITICAL(&s_state_mux);
    return ret;
}

int sched_runner_job_count(void)
{
    taskENTER_CRITICAL(&s_state_mux);
    int n = s_prog_valid ? s_prog.job_count : 0;
    taskEXIT_CRITICAL(&s_state_mux);
    return n;
}

/* Envelope provenance adapter (schedule_provenance_port.h). device_commands
 * splices workbook provenance into every publish envelope but cannot include
 * this component (sched_runner already REQUIRES device_commands — a direct
 * call would close the cycle), so app_main wires this translation instead.
 * Presence rides the snapshot's explicit has_workbook / macro_count — never
 * the CLI's "-" sentinel string. */
esp_err_t sched_runner_provenance_port(schedule_provenance_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_STATE;
    sched_header_t hdr;
    esp_err_t ret = sched_runner_header(&hdr);
    if (ret != ESP_OK) return ret;
    memset(out, 0, sizeof(*out));
    if (hdr.has_workbook) {
        snprintf(out->workbook_version_id, sizeof(out->workbook_version_id),
                 "%s", hdr.workbook);
    }
    out->macro_count = hdr.macro_count;
    for (int i = 0; i < hdr.macro_count && i < SCHEDULE_PROVENANCE_MAX_MACROS; i++) {
        snprintf(out->macros[i].id, sizeof(out->macros[i].id), "%s", hdr.macros[i].id);
        snprintf(out->macros[i].name, sizeof(out->macros[i].name), "%s", hdr.macros[i].name);
        snprintf(out->macros[i].filename, sizeof(out->macros[i].filename), "%s",
                 hdr.macros[i].filename);
    }
    return ESP_OK;
}

esp_err_t sched_runner_job_status(int idx, sched_job_status_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_STATE;
    taskENTER_CRITICAL(&s_state_mux);
    if (!s_prog_valid || idx < 0 || idx >= s_prog.job_count) {
        taskEXIT_CRITICAL(&s_state_mux);
        return ESP_ERR_NOT_FOUND;
    }
    const sched_job_t *job = &s_prog.jobs[idx];
    const char *name = sched_pool_str(&s_prog, job->name_off);
    snprintf(out->name, sizeof(out->name), "%s", name ? name : "?");
    taskENTER_CRITICAL(&s_stats_mux);
    out->stats.runs             = s_rt[idx].runs;
    out->stats.failures         = s_rt[idx].failures;
    out->stats.fail_streak      = s_rt[idx].fail_streak;
    out->stats.last_run_epoch   = s_rt[idx].last_run_epoch;
    out->stats.last_duration_ms = s_rt[idx].last_duration_ms;
    taskEXIT_CRITICAL(&s_stats_mux);
    out->stats.skipped = s_snap[idx].skipped;
    out->stats.skipped_saturated = s_snap[idx].skipped_saturated != 0;
    out->boot_pending  = s_snap[idx].boot_pending != 0;
    out->next_due_utc  = s_snap[idx].next_due_utc;
    taskEXIT_CRITICAL(&s_state_mux);
    return ESP_OK;
}

esp_err_t sched_runner_validate_file(const char *path, char *err, size_t err_cap)
{
    char *buf = NULL;
    size_t len = 0;
    esp_err_t r = read_schedule_file(path, &buf, &len);
    if (r != ESP_OK) {
        snprintf(err, err_cap, "%s: %s", path, esp_err_to_name(r));
        return r;
    }
    /* The program is ~15.7 KB of static-shaped state; heap for a one-shot
     * validation so the CLI never grows the runner's static footprint. */
    sched_program_t *tmp = malloc(sizeof(*tmp));
    if (tmp == NULL) {
        free(buf);
        snprintf(err, err_cap, "out of memory");
        return ESP_ERR_NO_MEM;
    }
    r = sched_compile_text(buf, len, tmp, err, err_cap);
    free(tmp);
    free(buf);
    return r;
}
