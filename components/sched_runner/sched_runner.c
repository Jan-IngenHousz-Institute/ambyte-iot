/*
 * sched_runner — task, lifecycle, timebase and job execution for the
 * declarative schedule. Replaces the Lua measurement task (see the epic
 * design: components/sched_spec compiles /littlefs/schedule.yaml or the
 * embedded default into a bounded sched_program_t; this task executes it).
 *
 * Timebase (plan §Timebase): monotonic esp_timer_get_time() for every wait
 * and deadline; wall clock via gettimeofday() (RTC-backed system time) +
 * timezone_localize() only when anchoring, which is what sched_due_poll /
 * sched_due_next do — the pure due model owns all wall-derived due times.
 * Clock-step detection compares monotonic vs wall elapsed each iteration; a
 * step over 2 s is logged and the very next poll re-anchors everything,
 * because the model's dues are absolute local instants (a forward jump turns
 * into counted skipped slots, a backward jump simply defers). The 60 s wait
 * ceiling bounds how long a step can go unnoticed.
 *
 * Overlap (skip/queue-one/reject) is execution semantics, owned here: the
 * single-task loop runs jobs synchronously, so the only firing that can find
 * a job mid-run is an on-demand dispatch. Scheduled busy slots are the due
 * model's late-grace/missed path, which hands out at most one firing per job
 * per poll — at most one deferred run by construction.
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
 * (Rationale comment moved verbatim from lua_runner.c.) */
#define SCHED_TASK_PRIORITY 10
/* Pin to APP_CPU (core 1). Wi-Fi (pinned core 0) and LwIP (prio 18) outrank
 * the measurement (prio 10), so an unpinned runner task that lands on core 0
 * gets preempted mid-UART-transaction → FSM/timing jitter. Pinning the
 * timing-sensitive measurement to core 1 keeps it off the radio's core; the
 * slack when it blocks is still reused by other tasks allowed on core 1.
 * (ESP32-S3 is dual-core; moved verbatim from lua_runner.c.) */
#define SCHED_TASK_CORE 1

/* The schedule lives on internal littlefs next to main.lua's old home;
 * power-loss-safe, no SD anywhere in the path (design decision 7). */
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

/* Failure-streak log throttle (runner-owned, never a YAML knob — flood
 * protection is not optional): log the first failure, every 300th while the
 * streak persists, and the recovery. Numbers from legacy_1Hz_spec.lua's fix
 * for the 536 K-line log flood. */
#define FAIL_STREAK_LOG_EVERY 300

/* Embedded default schedule, generated at configure time (see CMakeLists). */
#include "default_yaml_embed.h"

static TaskHandle_t s_task = NULL;

/* Stop flag polled by the loop and by every action between UART
 * transactions; set by sched_runner_stop(). */
static volatile bool s_should_stop = false;

/* Signaled by the task when it has fully exited, so stop() can wait and a
 * later start() cannot overlap two runner tasks. Created lazily. */
static SemaphoreHandle_t s_done_sem = NULL;
static StaticSemaphore_t s_done_sem_storage;

static sched_program_t s_prog;       /* the single static program (plan) */
static bool            s_prog_valid = false;
static sched_due_t     s_due;
static sched_source_t  s_source = { SCHED_SOURCE_NONE, "", "" };

/* Runner-owned execution counters; the due model owns `skipped` (mirrored
 * out on read). Indexed by job. Read by the CLI on another task — 32-bit
 * words are atomic on Xtensa and a status print tolerates a torn mix. */
typedef struct {
    uint32_t runs, failures, fail_streak;
    int64_t  last_run_epoch;
    uint32_t last_duration_ms;
} job_rt_t;
static job_rt_t s_rt[SCHED_SPEC_MAX_JOBS];

/* Dispatch queue + the queue-one deferred latch (both under s_dispatch_mux;
 * touched by the CLI and MQTT tasks). */
static portMUX_TYPE s_dispatch_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t s_dispatch_q[DISPATCH_QUEUE_LEN];
static int     s_dispatch_head = 0, s_dispatch_len = 0;
static int     s_running_job = -1;   /* written only on the runner task... */
static uint32_t s_deferred = 0;      /* ...except this: queue-one latch bits */

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
 * bug — the file is fixed at configure time and host-tested — so assert. */
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
    if (device_config_get_lat(&lat) == ESP_OK &&
        device_config_get_lon(&lon) == ESP_OK) {
        int tz;
        time_sync_get_location(NULL, NULL, &tz); /* keep the configured tz */
        time_sync_set_location(lat, lon, tz);
        ESP_LOGI(TAG, "location from device_config: lat=%.5f lon=%.5f", lat, lon);
    } else if (!s_loc_warned) {
        /* One WARN per boot, mirroring the Lua script's warning: sun triggers
         * and windows fall back to time_sync's compiled NL default. */
        ESP_LOGW(TAG, "no lat/lon in device_config — sun triggers use the "
                      "compiled default (52.173N 5.819E)");
        s_loc_warned = true;
    }
    (void)device_config_get_deployment(s_act_ctx.deployment,
                                       sizeof(s_act_ctx.deployment));
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
    job_rt_t *rt = &s_rt[j];
    if (s_should_stop) return;

    s_running_job = j;
    /* Snapshot the counters the $job.* placeholders resolve from. */
    s_act_ctx.job_idx     = j;
    s_act_ctx.job_name    = name;
    s_act_ctx.runs        = rt->runs;
    s_act_ctx.failures    = rt->failures;
    s_act_ctx.skipped     = s_due.jobs[j].skipped;
    s_act_ctx.fail_streak = rt->fail_streak;

    const int64_t t0_us = esp_timer_get_time();
    bool failed = false;
    for (int s = 0; s < job->step_count && !s_should_stop; s++) {
        const sched_step_t *step = &job->steps[s];
        esp_err_t err = step->action->run(&s_act_ctx, step, &s_prog);
        if (err != ESP_OK) {
            if (s_should_stop) break; /* a stop abort is not a job failure */
            failed = true;
            ESP_LOGW(TAG, "%s: step %s failed: %s",
                     name, step->action->name, esp_err_to_name(err));
            if (!step->continue_on_error) break;
        }
    }
    rt->last_duration_ms = (uint32_t)((esp_timer_get_time() - t0_us) / 1000);
    rt->last_run_epoch   = wall_now_utc();

    if (failed) {
        rt->failures++;
        rt->fail_streak++;
        if (rt->fail_streak == 1 || rt->fail_streak % FAIL_STREAK_LOG_EVERY == 0) {
            ESP_LOGW(TAG, "%s: failure streak %lu (throttled: next log at %lu)",
                     name, (unsigned long)rt->fail_streak,
                     (unsigned long)(rt->fail_streak / FAIL_STREAK_LOG_EVERY + 1) *
                         FAIL_STREAK_LOG_EVERY);
        }
    } else if (!s_should_stop) {
        rt->runs++;
        if (rt->fail_streak != 0) {
            ESP_LOGI(TAG, "%s: recovered after %lu consecutive failures",
                     name, (unsigned long)rt->fail_streak);
            rt->fail_streak = 0;
        }
    }
    s_running_job = -1;
    maybe_run_deferred(j);
}

/* ── dispatch (other tasks → runner task) ────────────────────────────── */

esp_err_t sched_runner_dispatch(const char *job_name)
{
    if (s_task == NULL) return ESP_ERR_INVALID_STATE;
    const sched_job_t *job = sched_find_job(&s_prog, job_name);
    if (job == NULL) return ESP_ERR_NOT_FOUND;
    int j = (int)(job - s_prog.jobs);

    esp_err_t ret = ESP_OK;
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
            ESP_LOGI(TAG, "%s: dispatch while busy — one firing deferred (queue-one)", job_name);
            break;
        case SCHED_OVERLAP_REJECT:
            s_rt[j].failures++;
            ESP_LOGW(TAG, "%s: dispatch while busy — rejected, counted as failure", job_name);
            break;
        case SCHED_OVERLAP_SKIP:
        default:
            ESP_LOGI(TAG, "%s: dispatch while busy — dropped (overlap skip)", job_name);
            break;
        }
    } else if (s_dispatch_len >= DISPATCH_QUEUE_LEN) {
        ret = ESP_ERR_NO_MEM;
    } else {
        s_dispatch_q[(s_dispatch_head + s_dispatch_len) % DISPATCH_QUEUE_LEN] = (uint8_t)j;
        s_dispatch_len++;
    }
    taskEXIT_CRITICAL(&s_dispatch_mux);

    if (ret == ESP_OK) xTaskNotifyGive(s_task);
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
    (void)arg;

    /* Due-model init happens on the task, after start(): boot triggers go
     * pending here, and start happens after clock trust (same call-site
     * contract as lua_runner), so boot means first trusted time. */
    sched_due_init(&s_due, &s_prog, runner_localize, NULL, wall_now_utc());

    const char *name = sched_pool_str(&s_prog, s_prog.name_off);
    ESP_LOGI(TAG, "running '%s' (%u jobs, source %s, sha256 %.12s…)",
             name ? name : "(unnamed)", (unsigned)s_prog.job_count,
             source_kind_str(s_source.kind), s_source.sha256);

    /* Boot banner continuity with main.lua:119 — the sunrise/sunset line is
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

    int64_t last_mono_us = esp_timer_get_time();
    int64_t last_wall_s  = wall_now_utc();

    for (;;) {
        if (s_should_stop) break;

        const int64_t now  = wall_now_utc();
        const int64_t mono = esp_timer_get_time();
        const int64_t wall_d = now - last_wall_s;
        const int64_t mono_d = (mono - last_mono_us) / 1000000;
        if (wall_d - mono_d > CLOCK_STEP_TOLERANCE_S ||
            mono_d - wall_d > CLOCK_STEP_TOLERANCE_S) {
            /* RTC set/corrected (set_time, SNTP). The due model re-anchors on
             * this very poll: dues are absolute local instants, so a forward
             * jump surfaces as counted skipped slots and a backward jump just
             * defers. No grid state lives outside the model. */
            ESP_LOGW(TAG, "clock stepped %lld s — re-anchoring wall triggers",
                     (long long)(wall_d - mono_d));
        }
        last_wall_s  = now;
        last_mono_us = mono;

        uint32_t mask = sched_due_poll(&s_due, now);
        for (int j = 0; j < s_prog.job_count; j++) {
            if (mask & (1u << j)) run_job(j);
        }
        drain_dispatch();
        if (s_should_stop) break;

        /* Soonest instant the poll can produce anything (firing or gate
         * entry); sched_due_next answers in local unix seconds. */
        const int64_t now2 = wall_now_utc();
        const int64_t L = timezone_localize(now2);
        int64_t wait_ms = LOOP_MAX_WAIT_MS;
        const int64_t next_local = sched_due_next(&s_due, now2);
        if (next_local >= 0) {
            int64_t d = (next_local - L) * 1000;
            if (d < 10) d = 10; /* overdue → spin once, don't busy-wait on 0 */
            if (d < wait_ms) wait_ms = d;
        }
        /* Woken early by stop() or dispatch(). */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS((uint32_t)wait_ms));
    }

    /* Stack high-water at exit: the number that trims SCHED_TASK_STACK after
     * the hardware soak (acceptance criterion 4). */
    ESP_LOGI(TAG, "stopped; stack high-water mark %u bytes of %u",
             (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)),
             (unsigned)SCHED_TASK_STACK);
    s_task = NULL;
    if (s_done_sem != NULL) xSemaphoreGive(s_done_sem);
    vTaskDelete(NULL);
}

/* ── lifecycle ───────────────────────────────────────────────────────── */

esp_err_t sched_runner_start(void)
{
    if (s_task != NULL) return ESP_ERR_INVALID_STATE;

    const size_t heap_before = esp_get_free_heap_size();

    /* Bind the action run functions once per boot; the table rows stay bound
     * across runner restarts (suspend/resume recompiles but the catalog is
     * firmware-fixed). */
    sched_runner_bind_actions();

    /* 1. Compile the installed schedule, else the embedded default. */
    char *buf = NULL;
    size_t len = 0;
    esp_err_t r = read_schedule_file(SCHED_PATH, &buf, &len);
    if (r == ESP_ERR_NOT_FOUND) {
        compile_embedded_or_die();
        s_source.kind = SCHED_SOURCE_EMBEDDED_DEFAULT;
        s_source.reason[0] = '\0';
        sha256_hex((const uint8_t *)default_yaml_start, default_yaml_size, s_source.sha256);
        ESP_LOGI(TAG, "no %s — running the embedded default", SCHED_PATH);
    } else if (r != ESP_OK) {
        ESP_LOGE(TAG, "%s unreadable (%s) — running the embedded default",
                 SCHED_PATH, esp_err_to_name(r));
        compile_embedded_or_die();
        s_source.kind = SCHED_SOURCE_EMBEDDED_FALLBACK;
        snprintf(s_source.reason, sizeof(s_source.reason), "read: %s", esp_err_to_name(r));
        sha256_hex((const uint8_t *)default_yaml_start, default_yaml_size, s_source.sha256);
    } else {
        char cerr[128];
        if (sched_compile_text(buf, len, &s_prog, cerr, sizeof(cerr)) == ESP_OK) {
            s_source.kind = SCHED_SOURCE_INSTALLED;
            s_source.reason[0] = '\0';
            sha256_hex((const uint8_t *)buf, len, s_source.sha256);
        } else {
            /* The line-numbered reason is the field diagnostic; keep it for
             * `schedule status` and run the known-good default. */
            ESP_LOGE(TAG, "%s: %s — running the embedded default", SCHED_PATH, cerr);
            compile_embedded_or_die();
            s_source.kind = SCHED_SOURCE_EMBEDDED_FALLBACK;
            snprintf(s_source.reason, sizeof(s_source.reason), "%s", cerr);
            sha256_hex((const uint8_t *)default_yaml_start, default_yaml_size, s_source.sha256);
        }
        free(buf);
    }
    s_prog_valid = true;

    /* The document header is provenance (JII idiom): carried, logged at
     * start, shown by `schedule status`, never acted on. */
    ESP_LOGI(TAG, "schedule id=%s version=%s workbook=%s",
             sched_pool_str(&s_prog, s_prog.id_off) ?: "-",
             sched_pool_str(&s_prog, s_prog.version_off) ?: "-",
             sched_pool_str(&s_prog, s_prog.workbook_version_id_off) ?: "-");

    /* 2. Reserve the shared trace/fallback buffer while the heap is
     * contiguous (same reasoning as lua_runner's module-register reserve;
     * ambit_ota and CLI raw runs share the buffer). */
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
    if (s_done_sem == NULL) {
        s_done_sem = xSemaphoreCreateBinaryStatic(&s_done_sem_storage);
        if (s_done_sem == NULL) return ESP_ERR_NO_MEM;
    } else {
        /* Drain any leftover give from a previous run so stop() waits on the
         * NEW task's exit, not a stale signal. */
        (void)xSemaphoreTake(s_done_sem, 0);
    }
    s_should_stop = false;
    s_running_job = -1;
    taskENTER_CRITICAL(&s_dispatch_mux);
    s_dispatch_head = s_dispatch_len = 0;
    s_deferred = 0;
    taskEXIT_CRITICAL(&s_dispatch_mux);
    memset(s_rt, 0, sizeof(s_rt));

    BaseType_t created = xTaskCreatePinnedToCore(
        sched_runner_task, SCHED_TASK_NAME, SCHED_TASK_STACK, NULL,
        SCHED_TASK_PRIORITY, &s_task, SCHED_TASK_CORE);
    if (created != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "runner started (heap %u → %u free)",
             (unsigned)heap_before, (unsigned)esp_get_free_heap_size());
    return ESP_OK;
}

esp_err_t sched_runner_stop(uint32_t wait_ms)
{
    if (s_task == NULL) return ESP_OK; /* nothing running */
    s_should_stop = true;
    xTaskNotifyGive(s_task);
    if (s_done_sem == NULL) return ESP_ERR_INVALID_STATE;
    /* Actions poll s_should_stop between UART transactions and inside poll
     * loops, so a stop normally lands within one poll interval (500 ms) plus
     * one transaction. A stop during a 30 s fetch returns TIMEOUT, as with
     * lua_runner, and the task exits on its own when the fetch returns. */
    if (xSemaphoreTake(s_done_sem, pdMS_TO_TICKS(wait_ms)) != pdTRUE) {
        ESP_LOGW(TAG, "stop: task still busy after %u ms — will exit later",
                 (unsigned)wait_ms);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

bool sched_runner_is_running(void)
{
    return s_task != NULL;
}

bool sched_runner_should_stop(void)
{
    return s_should_stop;
}

/* ── status queries (CLI) ────────────────────────────────────────────── */

void sched_runner_source(sched_source_t *out)
{
    if (out != NULL) *out = s_source;
}

esp_err_t sched_runner_stats(const char *job_name, sched_job_stats_t *out)
{
    if (!s_prog_valid || out == NULL) return ESP_ERR_INVALID_STATE;
    const sched_job_t *job = sched_find_job(&s_prog, job_name);
    if (job == NULL) return ESP_ERR_NOT_FOUND;
    int j = (int)(job - s_prog.jobs);
    out->runs             = s_rt[j].runs;
    out->failures         = s_rt[j].failures;
    out->fail_streak      = s_rt[j].fail_streak;
    out->skipped          = s_due.jobs[j].skipped;
    out->last_run_epoch   = s_rt[j].last_run_epoch;
    out->last_duration_ms = s_rt[j].last_duration_ms;
    return ESP_OK;
}

const sched_program_t *sched_runner_program(void)
{
    return s_prog_valid ? &s_prog : NULL;
}

int sched_runner_job_count(void)
{
    return s_prog_valid ? s_prog.job_count : 0;
}

esp_err_t sched_runner_job_status(int idx, sched_job_status_t *out)
{
    if (!s_prog_valid || out == NULL) return ESP_ERR_INVALID_STATE;
    if (idx < 0 || idx >= s_prog.job_count) return ESP_ERR_NOT_FOUND;
    const sched_job_t *job = &s_prog.jobs[idx];
    const char *name = sched_pool_str(&s_prog, job->name_off);
    snprintf(out->name, sizeof(out->name), "%s", name ? name : "?");
    out->stats.runs             = s_rt[idx].runs;
    out->stats.failures         = s_rt[idx].failures;
    out->stats.fail_streak      = s_rt[idx].fail_streak;
    out->stats.skipped          = s_due.jobs[idx].skipped;
    out->stats.last_run_epoch   = s_rt[idx].last_run_epoch;
    out->stats.last_duration_ms = s_rt[idx].last_duration_ms;

    /* dues are local unix seconds; convert with the offset valid NOW (good
     * enough for a status table — DST transitions move a display, not a
     * firing). */
    out->boot_pending = s_due.jobs[idx].boot_pending != 0;
    int64_t best = -1;
    for (int t = 0; t < job->trigger_count; t++) {
        int64_t d = s_due.jobs[idx].due[t];
        if (d >= 0 && (best < 0 || d < best)) best = d;
    }
    if (best >= 0) {
        out->next_due_utc = best - timezone_utc_offset_seconds(wall_now_utc());
    } else {
        out->next_due_utc = -1;
    }
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
    /* The program is ~13 KB of static-shaped state; heap for a one-shot
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
