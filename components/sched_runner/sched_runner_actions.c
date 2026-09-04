/*
 * sched_runner_actions.c — run functions for the action catalog, bound into
 * the sched_spec table by sched_runner_bind_actions(). Each is a fused,
 * task-level operation (the spec never sees trigger/poll/fetch loops), ported
 * from the Lua bindings/scripts it replaces:
 *
 *   ambit/trace        ← main.lua run_trace (ping gate, parallel trigger,
 *                        poll from 90 % of the estimate every 500 ms, fetch +
 *                        store on done, broken past est + deadline_margin)
 *   ambit/spectrum     ← l_ambit_spec + record_spectra (missing id = failure)
 *   ambit/leaf-temp    ← l_ambit_leaf_temp
 *   ambit/actinic      ← cmd_ambit_actinic, mandatory duration, off on stop
 *   device/status-report ← cmd_store_status_event (firmware heartbeat schema)
 *   db/store-event     ← l_db_store_event, tag MEASUREMENT, $-placeholders
 *   device/log, device/sleep
 *
 * Inputs arrive pre-validated and typed by the compiler; these functions
 * never parse. Every action polls sched_runner_should_stop() between UART
 * transactions so sched_runner_stop() lands within one poll interval plus
 * one transaction. Sensor actions are ping-gated: the 5-minute negative ping
 * cache in uart_sensors is the absent-channel flood fix and stays in
 * firmware, so a schedule that names an empty channel costs one ping per
 * cache TTL, not one wake burst per firing.
 */

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ambit_protocol.h"   /* AMBIT_ASYNC_* run-state constants */
#include "ambit_trace.h"
#include "device_commands.h"
#include "device_config.h"
#include "sched_runner.h"
#include "sched_runner_priv.h"
#include "time_sync.h"
#include "timezone.h"

#define TAG "sched_act"

/* ── parallel-run tuning, from main.lua:37-40 ─────────────────────────── */
#define POLL_INTERVAL_MS   500    /* gap between poll sweeps */
#define POLL_START_PCT     90     /* don't poll until 90 % of the estimate elapsed */
#define TRACE_TRIGGER_TIMEOUT_MS 3000   /* covers wake + ack only */
#define TRACE_FETCH_TIMEOUT_MS  30000   /* must cover the array stream */
#define TRACE_POLL_TIMEOUT_MS   400     /* a measuring AMBIT fails fast */

/* ── step input access ────────────────────────────────────────────────── */

static const sched_entry_t *step_input(const sched_step_t *step,
                                       const sched_program_t *prog,
                                       const char *name)
{
    for (int e = 0; e < step->entry_count; e++) {
        const sched_entry_t *en = &prog->entries[step->entry_start + e];
        if (strcmp(step->action->inputs[en->input_idx].name, name) == 0) return en;
    }
    return NULL;
}

/* Channel mask. n == 0 (input absent) means "all channels that answer a
 * ping" — the document is experiment intent, channel topology is device
 * state, so ping-gating below decides what actually runs either way. */
static uint8_t channels_mask(const sched_step_t *step, const sched_program_t *prog)
{
    const sched_entry_t *en = step_input(step, prog, "channels");
    if (en == NULL || en->type != SCHED_VAL_CHANNELS || en->u.chans.n == 0) {
        return (1u << UART_SENSOR_NUM_CHANNELS) - 1u;
    }
    uint8_t mask = 0;
    for (uint8_t i = 0; i < en->u.chans.n; i++) mask |= (uint8_t)(1u << en->u.chans.v[i]);
    return mask;
}

static bool ch_present(uint8_t ch)
{
    bool connected = false;
    cmd_result_t r = cmd_uart_ping(ch, &connected);
    return r.status == ESP_OK && connected;
}

/* Interruptible sleep in 100 ms chunks; returns false when the runner is
 * stopping. The chunk bounds the stop latency, not the timing accuracy. */
static bool act_sleep_ms(uint32_t ms)
{
    while (ms > 0) {
        if (sched_runner_should_stop()) return false;
        uint32_t chunk = ms > 100 ? 100 : ms;
        vTaskDelay(pdMS_TO_TICKS(chunk));
        ms -= chunk;
    }
    return true;
}

static int64_t now_wall_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/* ── ambit/trace ───────────────────────────────────────────────────────── */

/* Caller-owned per-channel trigger→fetch attribution. Static (not stack):
 * four pendings carry the 544 B cmd buffer + 512 B protocol ref each, which
 * would eat most of the runner's 8 KiB stack. Only the runner task touches
 * them; the Lua runner keeps its own copies while it still compiles. */
static ambit_trace_pending_t s_pend[UART_SENSOR_NUM_CHANNELS];

static const sched_protocol_t *find_protocol(const sched_program_t *prog,
                                             const char *name)
{
    for (int p = 0; p < prog->protocol_count; p++) {
        const char *pn = sched_pool_str(prog, prog->protocols[p].name_off);
        if (pn != NULL && strcmp(pn, name) == 0) return &prog->protocols[p];
    }
    return NULL;
}

static esp_err_t act_ambit_trace(void *vctx, const sched_step_t *step,
                                 const sched_program_t *prog)
{
    sched_runner_act_ctx_t *ctx = vctx;
    const sched_entry_t *e_proto  = step_input(step, prog, "protocol");
    const sched_entry_t *e_hold   = step_input(step, prog, "hold_window");
    const sched_entry_t *e_tag    = step_input(step, prog, "tag");
    const sched_entry_t *e_margin = step_input(step, prog, "deadline_margin");

    const char *pname = (e_proto && e_proto->type == SCHED_VAL_STR)
                            ? sched_pool_str(prog, e_proto->u.str_off) : NULL;
    const sched_protocol_t *proto = pname ? find_protocol(prog, pname) : NULL;
    if (proto == NULL) {
        ESP_LOGW(TAG, "%s: unknown protocol '%s'", ctx->job_name, pname ? pname : "?");
        return ESP_ERR_NOT_FOUND;
    }
    /* tag is what lands in protocol_ref.protocol (the v3 metadata.protocol
     * column the Databricks filters match on); default = protocol name. */
    const char *tag = (e_tag && e_tag->type == SCHED_VAL_STR)
                          ? sched_pool_str(prog, e_tag->u.str_off) : NULL;
    if (tag == NULL) tag = pname;
    const bool hold_window = e_hold != NULL && e_hold->u.i != 0;
    const int64_t margin_ms = e_margin ? e_margin->u.i : 15000;

    /* Six-field segments map one-to-one; both structs are the AMBIT wire
     * format, T1's in firmware order, T2's compile-checked. */
    ambit_trace_segment_t segs[SCHED_SPEC_MAX_SEGMENTS];
    const int nseg = proto->segment_count;
    for (int i = 0; i < nseg; i++) {
        segs[i] = (ambit_trace_segment_t) {
            .type        = proto->segments[i].type,
            .far_red     = proto->segments[i].far_red != 0,
            .pulses      = proto->segments[i].pulses,
            .freq        = proto->segments[i].freq,
            .actinic     = proto->segments[i].actinic,
            .subsampling = proto->segments[i].subsampling,
        };
    }
    const int64_t est_ms = ambit_trace_estimate_ms(segs, (size_t)nseg);
    const uint8_t mask = channels_mask(step, prog);

    if (hold_window) device_commands_measurement_begin();

    ambit_trace_options_t opts = {
        .persist         = proto->persist,
        .allow_interrupt = proto->allow_interrupt != 0,
        .timeout_ms      = TRACE_TRIGGER_TIMEOUT_MS,
        .protocol_ref    = { { 0 } },
    };
    snprintf(opts.protocol_ref.protocol, sizeof(opts.protocol_ref.protocol),
             "%s", tag);

    int pending_count = 0;
    int64_t t0[UART_SENSOR_NUM_CHANNELS] = { 0 };
    for (uint8_t ch = 0; ch < UART_SENSOR_NUM_CHANNELS; ch++) {
        if (!(mask & (1u << ch))) continue;
        if (sched_runner_should_stop()) break;
        if (!ch_present(ch)) continue;
        cmd_result_t r = ambit_trace_trigger(ch, segs, (size_t)nseg, &opts, &s_pend[ch]);
        if (r.status == ESP_OK) {
            t0[ch] = esp_timer_get_time() / 1000;
            pending_count++;
        } else {
            ESP_LOGW(TAG, "%s ch%u: trigger failed: %s", tag, ch, r.message);
        }
    }

    int fetched = 0, chan_failed = 0;
    while (pending_count > 0) {
        if (!act_sleep_ms(POLL_INTERVAL_MS)) break; /* stop: leave runs buffered */
        int64_t now = esp_timer_get_time() / 1000;
        for (uint8_t ch = 0; ch < UART_SENSOR_NUM_CHANNELS; ch++) {
            if (t0[ch] == 0) continue;
            int64_t elapsed = now - t0[ch];
            if (elapsed < est_ms * POLL_START_PCT / 100) continue;
            uint8_t st = 0xFF;
            cmd_result_t pr = cmd_ambit_poll(ch, &st, TRACE_POLL_TIMEOUT_MS);
            if (pr.status == ESP_OK && st == AMBIT_ASYNC_DONE) {
                ambit_trace_result_t res;
                cmd_result_t fr = ambit_trace_fetch(ch, &s_pend[ch], true,
                                                    TRACE_FETCH_TIMEOUT_MS, &res);
                if (fr.status == ESP_OK) {
                    ESP_LOGI(TAG, "%s ch%u: %u points, %.1fC, stored %lld",
                             tag, ch, (unsigned)res.points, res.leaf_temp,
                             (long long)res.measure_id);
                    fetched++;
                } else {
                    ESP_LOGW(TAG, "%s ch%u: fetch failed: %s", tag, ch, fr.message);
                    chan_failed++;
                }
                t0[ch] = 0;
                pending_count--;
            } else if (pr.status == ESP_OK && st == AMBIT_ASYNC_ERROR) {
                ESP_LOGW(TAG, "%s ch%u: ambit reported run error", tag, ch);
                t0[ch] = 0;
                pending_count--;
                chan_failed++;
            } else if (elapsed > est_ms + margin_ms) {
                ESP_LOGW(TAG, "%s ch%u: no result after %lldms — ambit broken?",
                         tag, ch, (long long)elapsed);
                t0[ch] = 0;
                pending_count--;
                chan_failed++;
            }
            /* else idle/busy: keep waiting */
        }
    }

    if (hold_window) device_commands_measurement_end();

    /* Failure accounting: "no AMBIT responded" and "every triggered channel
     * failed" are job failures; a partial round (one of two channels stored)
     * is a success with a warning trail, as under main.lua. A stop abort is
     * neither (run_job skips counting when should_stop is set). */
    if (pending_count + fetched + chan_failed == 0) return ESP_FAIL;
    if (fetched == 0 && !sched_runner_should_stop()) return ESP_FAIL;
    return ESP_OK;
}

/* ── ambit/spectrum, ambit/leaf-temp ───────────────────────────────────── */

/* Shared store shape from l_ambit_spec / l_ambit_leaf_temp: provenance
 * metadata from the cached device info, store through the fused helper. */
static int64_t store_small(uint8_t ch, const char *cmd_raw, const char *payload,
                           int64_t start_ms, int64_t end_ms)
{
    ambit_device_info_t info;
    const char *dev = ambit_device_name(ch, &info);
    char meta[200];
    int o = snprintf(meta, sizeof meta, "{");
    o += ambit_config_kvs(&info, meta + o, sizeof meta - o);
    snprintf(meta + o, sizeof meta - o, "}");
    return ambit_store_small(ch, dev, cmd_raw, meta, start_ms, end_ms, payload);
}

static esp_err_t act_ambit_spectrum(void *vctx, const sched_step_t *step,
                                    const sched_program_t *prog)
{
    sched_runner_act_ctx_t *ctx = vctx;
    const uint8_t mask = channels_mask(step, prog);
    int present = 0, stored_ok = 0;
    for (uint8_t ch = 0; ch < UART_SENSOR_NUM_CHANNELS; ch++) {
        if (!(mask & (1u << ch))) continue;
        if (sched_runner_should_stop()) break;
        if (!ch_present(ch)) continue;
        present++;
        int64_t start_ms = now_wall_ms();
        uint16_t spec[10] = { 0 };
        float par = 0;
        cmd_result_t r = cmd_ambit_get_spec(ch, spec, &par);
        int64_t end_ms = now_wall_ms();
        if (r.status != ESP_OK) {
            ESP_LOGW(TAG, "%s: spectra ch%u: read failed: %s", ctx->job_name, ch, r.message);
            continue;
        }
        char payload[160];
        snprintf(payload, sizeof payload,
                 "{\"spec\":[%u,%u,%u,%u,%u,%u,%u,%u,%u,%u],\"par\":%.2f}",
                 spec[0], spec[1], spec[2], spec[3], spec[4],
                 spec[5], spec[6], spec[7], spec[8], spec[9], (double)par);
        int64_t mid = store_small(ch, "get_par", payload, start_ms, end_ms);
        /* legacy_1Hz_spec.lua:185-192: a missing stored id is a failure, not
         * a warning — a measurement that didn't persist didn't happen. */
        if (mid < 0) {
            ESP_LOGW(TAG, "%s: spectra ch%u: PAR=%.2f store failed", ctx->job_name, ch, (double)par);
            continue;
        }
        ESP_LOGI(TAG, "%s: spectra ch%u: PAR=%.2f id=%lld",
                 ctx->job_name, ch, (double)par, (long long)mid);
        stored_ok++;
    }
    if (present == 0) {
        ESP_LOGW(TAG, "%s: spectra: no AMBIT responded", ctx->job_name);
        return ESP_FAIL;
    }
    return stored_ok == present ? ESP_OK : ESP_FAIL;
}

static esp_err_t act_ambit_leaf_temp(void *vctx, const sched_step_t *step,
                                     const sched_program_t *prog)
{
    sched_runner_act_ctx_t *ctx = vctx;
    const uint8_t mask = channels_mask(step, prog);
    int present = 0, stored_ok = 0;
    for (uint8_t ch = 0; ch < UART_SENSOR_NUM_CHANNELS; ch++) {
        if (!(mask & (1u << ch))) continue;
        if (sched_runner_should_stop()) break;
        if (!ch_present(ch)) continue;
        present++;
        int64_t start_ms = now_wall_ms();
        float leaf = 0, chip = 0;
        cmd_result_t r = cmd_ambit_get_temp(ch, &leaf, &chip);
        int64_t end_ms = now_wall_ms();
        if (r.status != ESP_OK) {
            ESP_LOGW(TAG, "%s: leaf-temp ch%u: read failed: %s", ctx->job_name, ch, r.message);
            continue;
        }
        char payload[64];
        snprintf(payload, sizeof payload, "{\"leaf\":%.2f,\"chip\":%.2f}",
                 (double)leaf, (double)chip);
        int64_t mid = store_small(ch, "get_temp", payload, start_ms, end_ms);
        if (mid < 0) {
            ESP_LOGW(TAG, "%s: leaf-temp ch%u: store failed", ctx->job_name, ch);
            continue;
        }
        stored_ok++;
    }
    if (present == 0) return ESP_FAIL;
    return stored_ok == present ? ESP_OK : ESP_FAIL;
}

/* ── ambit/actinic ─────────────────────────────────────────────────────── */

static esp_err_t act_ambit_actinic(void *vctx, const sched_step_t *step,
                                   const sched_program_t *prog)
{
    sched_runner_act_ctx_t *ctx = vctx;
    const sched_entry_t *e_level = step_input(step, prog, "level");
    const sched_entry_t *e_dur   = step_input(step, prog, "duration");
    const int32_t level = e_level ? (int32_t)e_level->u.i : 0;
    const int64_t duration_ms = e_dur ? e_dur->u.i : 0;
    const uint8_t mask = channels_mask(step, prog);
    if (duration_ms <= 0) return ESP_ERR_INVALID_ARG; /* compiler enforces ≥ 1 */

    /* Cmd 4 type 5 pulses the actinic LED at `var` current for `var2`×100 ms,
     * self-timed on the AMBIT. var2 is a byte, so a long duration is a chain
     * of ≤25.5 s pulses with a stop check between them. */
    #define ACTINIC_CHUNK_MS 25500
    uint8_t dac[UART_SENSOR_NUM_CHANNELS] = { 0 };
    bool lit[UART_SENSOR_NUM_CHANNELS] = { false };
    esp_err_t result = ESP_OK;

    for (uint8_t ch = 0; ch < UART_SENSOR_NUM_CHANNELS; ch++) {
        if (!(mask & (1u << ch))) continue;
        if (!ch_present(ch)) continue;
        ambit_device_info_t info;
        cmd_result_t ir = cmd_ambit_device_info(ch, &info);
        if (ir.status != ESP_OK || !info.valid) {
            ESP_LOGW(TAG, "%s: actinic ch%u: no device info (%s)",
                     ctx->job_name, ch, ir.message);
            result = ESP_FAIL;
            continue;
        }
        /* WRENCH convention (ambit_actinic_to_dac): negative = raw DAC,
         * positive = PAR µmol converted by the calibration coefficient. */
        dac[ch] = ambit_actinic_to_dac((int16_t)level, info.actinic_coef);
    }

    int64_t remaining = duration_ms;
    while (remaining > 0 && !sched_runner_should_stop()) {
        const int chunk = remaining > ACTINIC_CHUNK_MS ? ACTINIC_CHUNK_MS : (int)remaining;
        for (uint8_t ch = 0; ch < UART_SENSOR_NUM_CHANNELS; ch++) {
            if (!(mask & (1u << ch)) || !ch_present(ch)) continue;
            cmd_result_t r = cmd_ambit_actinic(ch, 5, dac[ch], (uint8_t)(chunk / 100));
            if (r.status != ESP_OK) {
                ESP_LOGW(TAG, "%s: actinic ch%u: %s", ctx->job_name, ch, r.message);
                result = ESP_FAIL;
            } else {
                lit[ch] = true;
            }
        }
        if (!act_sleep_ms((uint32_t)chunk)) break; /* stop requested */
        remaining -= chunk;
    }

    /* Cleanup runs even on stop: a job must never leave the light on. The
     * pulse self-terminates, so "off" is a zero-current 100 ms pulse on every
     * channel we lit — best-effort, the AMBIT has no explicit off verb. */
    for (uint8_t ch = 0; ch < UART_SENSOR_NUM_CHANNELS; ch++) {
        if (!lit[ch]) continue;
        (void)cmd_ambit_actinic(ch, 5, 0, 1);
    }
    #undef ACTINIC_CHUNK_MS
    return result;
}

/* ── device/status-report ──────────────────────────────────────────────── */

static esp_err_t act_status_report(void *vctx, const sched_step_t *step,
                                   const sched_program_t *prog)
{
    sched_runner_act_ctx_t *ctx = vctx;
    if (step_input(step, prog, "tags") != NULL) {
        /* v1: the STATUS payload has no room for schedule tags (319 B
         * headroom, device_commands.c budget comment) — accepted and ignored
         * rather than failing a schedule that carries them. */
        ESP_LOGW(TAG, "%s: status-report tags are not supported in v1 — ignored",
                 ctx->job_name);
    }
    cmd_result_t r = cmd_store_status_event();
    if (r.status != ESP_OK) {
        ESP_LOGW(TAG, "%s: status-report: %s", ctx->job_name, r.message);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ── db/store-event ────────────────────────────────────────────────────── */

/* Minimal JSON object writer for the compiler-validated flat maps: keys are
 * schedule-authored strings, values typed scalars with $-placeholders
 * resolved on device (design catalog row). Bounded: ≤16 keys per map, values
 * out of the 4 KiB string pool. */

typedef struct {
    char  *buf;
    size_t cap, len;
    bool   overflow;
} jw_t;

static void jw_raw(jw_t *w, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(w->buf + (w->len < w->cap ? w->len : w->cap),
                      w->len < w->cap ? w->cap - w->len : 0, fmt, ap);
    va_end(ap);
    if (n < 0 || w->len + (size_t)n >= w->cap) w->overflow = true;
    if (n > 0) w->len += (size_t)n;
}

static void jw_str(jw_t *w, const char *s)
{
    jw_raw(w, "\"");
    for (const char *p = s; *p != '\0' && !w->overflow; p++) {
        if (*p == '"' || *p == '\\') jw_raw(w, "\\%c", *p);
        else jw_raw(w, "%c", *p);
    }
    jw_raw(w, "\"");
}

/* Resolve a $-placeholder to its typed JSON token. Unset device facts
 * resolve to "" / 0 rather than failing the event — a partially-provisioned
 * unit still reports. */
static void jw_placeholder(jw_t *w, const char *ph, const sched_runner_act_ctx_t *ctx)
{
    if (strcmp(ph, "$deployment") == 0) {
        jw_str(w, ctx->deployment);
    } else if (strcmp(ph, "$lat") == 0 || strcmp(ph, "$lon") == 0) {
        double lat, lon;
        time_sync_get_location(&lat, &lon, NULL);
        jw_raw(w, "%.5f", strcmp(ph, "$lat") == 0 ? lat : lon);
    } else if (strcmp(ph, "$tz") == 0) {
        char tz[48] = "";
        (void)device_config_get_timezone(tz, sizeof(tz));
        jw_str(w, tz);
    } else if (strcmp(ph, "$boot_epoch") == 0) {
        jw_raw(w, "%lld", (long long)ctx->boot_epoch);
    } else if (strcmp(ph, "$uptime_ms") == 0) {
        jw_raw(w, "%lld", (long long)(esp_timer_get_time() / 1000));
    } else if (strcmp(ph, "$sd_ready") == 0) {
        bool ready = false;
        (void)cmd_sd_ready(&ready);
        jw_raw(w, "%s", ready ? "true" : "false");
    } else if (strcmp(ph, "$job.runs") == 0) {
        jw_raw(w, "%lu", (unsigned long)ctx->runs);
    } else if (strcmp(ph, "$job.failures") == 0) {
        jw_raw(w, "%lu", (unsigned long)ctx->failures);
    } else if (strcmp(ph, "$job.skipped") == 0) {
        jw_raw(w, "%lu", (unsigned long)ctx->skipped);
    } else if (strcmp(ph, "$job.fail_streak") == 0) {
        jw_raw(w, "%lu", (unsigned long)ctx->fail_streak);
    } else {
        jw_str(w, ph); /* compiler validated the set; unreachable */
    }
}

static void jw_value(jw_t *w, const sched_entry_t *en, const sched_program_t *prog,
                     const sched_runner_act_ctx_t *ctx)
{
    switch (en->type) {
    case SCHED_VAL_INT:   jw_raw(w, "%lld", (long long)en->u.i); break;
    case SCHED_VAL_FLOAT: jw_raw(w, "%g", en->u.f); break;
    case SCHED_VAL_BOOL:  jw_raw(w, "%s", en->u.i ? "true" : "false"); break;
    default: {
        const char *s = sched_pool_str(prog, en->u.str_off);
        if (s != NULL && s[0] == '$') jw_placeholder(w, s, ctx);
        else jw_str(w, s ? s : "");
        break;
    }
    }
}

/* Serialize one flat map input (data/metadata). Returns false on overflow. */
static bool build_map_json(char *buf, size_t cap, const char *input_name,
                           const char *extra_key, const char *extra_val,
                           const sched_step_t *step, const sched_program_t *prog,
                           const sched_runner_act_ctx_t *ctx)
{
    jw_t w = { buf, cap, 0, false };
    jw_raw(&w, "{");
    bool first = true;
    if (extra_key != NULL) {
        jw_str(&w, extra_key);
        jw_raw(&w, ":");
        jw_str(&w, extra_val);
        first = false;
    }
    for (int e = 0; e < step->entry_count; e++) {
        const sched_entry_t *en = &prog->entries[step->entry_start + e];
        const sched_input_decl_t *decl = &step->action->inputs[en->input_idx];
        if (strcmp(decl->name, input_name) != 0 || en->key_off == SCHED_POOL_NONE) {
            continue;
        }
        jw_raw(&w, "%s", first ? "" : ",");
        jw_str(&w, sched_pool_str(prog, en->key_off));
        jw_raw(&w, ":");
        jw_value(&w, en, prog, ctx);
        first = false;
    }
    jw_raw(&w, "}");
    return !w.overflow;
}

static esp_err_t act_db_store_event(void *vctx, const sched_step_t *step,
                                    const sched_program_t *prog)
{
    sched_runner_act_ctx_t *ctx = vctx;
    char chan[12] = "";
    const sched_entry_t *e_chan = step_input(step, prog, "channel");
    if (e_chan != NULL) snprintf(chan, sizeof chan, "uart_%d", (int)e_chan->u.i);

    const sched_entry_t *e_kind = step_input(step, prog, "kind");
    const char *kind = (e_kind && e_kind->type == SCHED_VAL_STR)
                           ? sched_pool_str(prog, e_kind->u.str_off) : NULL;

    int64_t measure_id = 0;
    cmd_result_t idr = cmd_next_measure_id(&measure_id);
    if (idr.status != ESP_OK) {
        ESP_LOGW(TAG, "%s: store-event: no measure id: %s", ctx->job_name, idr.message);
        return ESP_FAIL;
    }

    /* kind stamps into data.kind (design catalog row). 768 B covers 16 keys
     * of realistic field data with placeholder expansions; overflow fails the
     * step loudly instead of storing a torn object. */
    char payload[768], metadata[768];
    if (!build_map_json(payload, sizeof payload, "data", "kind", kind,
                        step, prog, ctx)) {
        ESP_LOGW(TAG, "%s: store-event: data map too large", ctx->job_name);
        return ESP_FAIL;
    }
    const bool has_meta = step_input(step, prog, "metadata") != NULL;
    if (has_meta && !build_map_json(metadata, sizeof metadata, "metadata",
                                    NULL, NULL, step, prog, ctx)) {
        ESP_LOGW(TAG, "%s: store-event: metadata map too large", ctx->job_name);
        return ESP_FAIL;
    }

    const int64_t now = now_wall_ms();
    measurement_event_desc_t d = {
        .measure_id    = measure_id,
        .channel       = chan,
        .tag           = MEASUREMENT_TAG_MEASUREMENT,
        .start_ms      = now,
        .end_ms        = now,
        .metadata_json = has_meta ? metadata : NULL,
        .payload_json  = payload,
    };
    cmd_result_t r = cmd_store_event(&d);
    if (r.status != ESP_OK) {
        ESP_LOGW(TAG, "%s: store-event: %s", ctx->job_name, r.message);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "%s: stored event id=%lld %s", ctx->job_name,
             (long long)measure_id, payload);
    return ESP_OK;
}

/* ── device/log, device/sleep ──────────────────────────────────────────── */

static esp_err_t act_device_log(void *vctx, const sched_step_t *step,
                                const sched_program_t *prog)
{
    sched_runner_act_ctx_t *ctx = vctx;
    const sched_entry_t *e_msg = step_input(step, prog, "message");
    const char *msg = (e_msg && e_msg->type == SCHED_VAL_STR)
                          ? sched_pool_str(prog, e_msg->u.str_off) : "(empty)";
    ESP_LOGI(TAG, "%s: %s", ctx->job_name, msg);
    return ESP_OK;
}

static esp_err_t act_device_sleep(void *vctx, const sched_step_t *step,
                                  const sched_program_t *prog)
{
    (void)vctx;
    const sched_entry_t *e_dur = step_input(step, prog, "duration");
    const int64_t ms = e_dur ? e_dur->u.i : 0;
    if (ms <= 0) return ESP_ERR_INVALID_ARG;
    (void)act_sleep_ms((uint32_t)ms); /* honours stop; run_job sees the flag */
    return ESP_OK;
}

/* ── binding ───────────────────────────────────────────────────────────── */

void sched_runner_bind_actions(void)
{
    static bool s_bound = false;
    if (s_bound) return;
    /* Every catalog action must have a run function before the first start;
     * a miss is a code bug, not a runtime condition. */
    ESP_ERROR_CHECK(sched_action_bind("ambit/trace",          &s_act_ctx, act_ambit_trace));
    ESP_ERROR_CHECK(sched_action_bind("ambit/spectrum",       &s_act_ctx, act_ambit_spectrum));
    ESP_ERROR_CHECK(sched_action_bind("ambit/leaf-temp",      &s_act_ctx, act_ambit_leaf_temp));
    ESP_ERROR_CHECK(sched_action_bind("ambit/actinic",        &s_act_ctx, act_ambit_actinic));
    ESP_ERROR_CHECK(sched_action_bind("device/status-report", &s_act_ctx, act_status_report));
    ESP_ERROR_CHECK(sched_action_bind("db/store-event",       &s_act_ctx, act_db_store_event));
    ESP_ERROR_CHECK(sched_action_bind("device/log",           &s_act_ctx, act_device_log));
    ESP_ERROR_CHECK(sched_action_bind("device/sleep",         &s_act_ctx, act_device_sleep));
    s_bound = true;
}
