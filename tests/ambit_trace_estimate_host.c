/*
 * ambit_trace_estimate_host.c — host unit runner for the AMBIT trace run-time
 * estimate (tests/test_ambit_trace_estimate.py compiles it; pattern:
 * tests/schedule_light_cleanup_host.c — the production functions are extracted
 * from components/ambit_trace/ambit_trace.c into production.inc so the ESP-IDF
 * heap/semaphore half of that file stays out of the host binary).
 *
 * Two acquisition engines, two time bases (ambit_trace.h): below AMBIT fw
 * 1.4.0 the ADPD free-runs and the nominal Σ pulses/freq + 300 ms/segment
 * overshoots the tick_factor-fast run; from 1.4.0 the EXT_SYNC engine paces
 * every pulse at exactly 1/freq plus ~0.5 s of setup and warm-up, so the
 * shipped 45-pulse SS run grows from ~41.2 s to ~44.4 s. The scheduler never
 * polls before the estimate (the AMBIT cannot answer mid-run), so the estimate
 * has to bound the slower engine's run and the gateway has to pick the engine
 * per channel — Nergena 2026-10-05 lost channel 0's SS traces to a 90 % poll
 * start on the free-run estimate.
 *
 * Checks:
 *   - engine selection from the cached cmd 33/2 identity: thresholds around
 *     1.4.0, unknown/invalid identity → paced (the longer estimate);
 *   - SS and MPF of schedule/default.yaml, compiled by the production schedule
 *     compiler and mapped to wire segments exactly as act_ambit_trace does,
 *     estimate at 45,300 / 45,600 ms and 10,800 / 10,100 ms (free-run / paced),
 *     and each engine's estimate bounds its measured run;
 *   - the legacy entry point is the free-run estimate;
 *   - every protocol of every catalog schedule (argv) stays under the 65 s
 *     maintenance settle (ambit_ota.c AMBIT_IDLE_SETTLE_MS) on both engines.
 *
 * Prints "AMBIT_TRACE_ESTIMATE_HOST_OK <checks>" and exits 0 when all pass.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ambit_trace.h"
#include "sched_spec.h"

#include "production.inc"

static int s_checks, s_fails;
#define CHECK(cond)                                                     \
    do {                                                                \
        s_checks++;                                                     \
        if (!(cond)) {                                                  \
            s_fails++;                                                  \
            printf("FAIL %d: %s\n", __LINE__, #cond);                   \
        }                                                               \
    } while (0)

/* Measured wall-clock runs of the shipped SS protocol (45 pulses at 1 Hz). */
#define SS_FREE_RUN_MEASURED_MS 41200 /* AMBIT 1.3.0: ADPD period counter, tick_factor */
#define SS_PACED_MEASURED_MS    44400 /* AMBIT 1.4.0-rc.1: EXT_SYNC, ESP clock */
/* ambit_ota.c AMBIT_IDLE_SETTLE_MS: maintenance waits this long for a running
 * trace before probing the UART. No catalog protocol may estimate past it. */
#define MAINTENANCE_SETTLE_MS   65000

static char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)n + 1);
    if (buf == NULL) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    *len = got;
    return buf;
}

static const sched_protocol_t *protocol(const sched_program_t *p, const char *name)
{
    for (int i = 0; i < p->protocol_count; i++) {
        const char *pn = sched_pool_str(p, p->protocols[i].name_off);
        if (pn != NULL && strcmp(pn, name) == 0) return &p->protocols[i];
    }
    return NULL;
}

/* The same six-field mapping act_ambit_trace performs before triggering. */
static size_t to_segments(const sched_protocol_t *proto, ambit_trace_segment_t *out)
{
    for (int i = 0; i < proto->segment_count; i++) {
        out[i] = (ambit_trace_segment_t) {
            .type        = proto->segments[i].type,
            .far_red     = proto->segments[i].far_red != 0,
            .pulses      = proto->segments[i].pulses,
            .freq        = proto->segments[i].freq,
            .actinic     = proto->segments[i].actinic,
            .subsampling = proto->segments[i].subsampling,
        };
    }
    return (size_t)proto->segment_count;
}

static ambit_trace_engine_t engine_of(const char *fw_version)
{
    ambit_device_info_t info;
    memset(&info, 0, sizeof info);
    info.valid = true;
    snprintf(info.fw_version, sizeof info.fw_version, "%s", fw_version);
    return ambit_trace_engine_for(&info);
}

static void test_engine_selection(void)
{
    CHECK(ambit_trace_engine_for(NULL) == AMBIT_TRACE_ENGINE_PACED);
    ambit_device_info_t unknown;
    memset(&unknown, 0, sizeof unknown); /* fetch failed: valid = false */
    CHECK(ambit_trace_engine_for(&unknown) == AMBIT_TRACE_ENGINE_PACED);
    CHECK(engine_of("") == AMBIT_TRACE_ENGINE_PACED);
    CHECK(engine_of("ambit") == AMBIT_TRACE_ENGINE_PACED);
    CHECK(engine_of("1") == AMBIT_TRACE_ENGINE_PACED);

    CHECK(engine_of("0.9.7") == AMBIT_TRACE_ENGINE_FREE_RUN);
    CHECK(engine_of("1.1.4") == AMBIT_TRACE_ENGINE_FREE_RUN);
    CHECK(engine_of("1.2.0") == AMBIT_TRACE_ENGINE_FREE_RUN);
    CHECK(engine_of("1.3.0") == AMBIT_TRACE_ENGINE_FREE_RUN);
    CHECK(engine_of("1.3.9") == AMBIT_TRACE_ENGINE_FREE_RUN);

    CHECK(engine_of("1.4.0") == AMBIT_TRACE_ENGINE_PACED);
    CHECK(engine_of("1.4.1") == AMBIT_TRACE_ENGINE_PACED);
    CHECK(engine_of("1.10.0") == AMBIT_TRACE_ENGINE_PACED);
    CHECK(engine_of("2.0.0") == AMBIT_TRACE_ENGINE_PACED);
}

static void test_default_schedule(const sched_program_t *p)
{
    ambit_trace_segment_t segs[SCHED_SPEC_MAX_SEGMENTS];

    const sched_protocol_t *ss = protocol(p, "SS");
    CHECK(ss != NULL);
    if (ss != NULL) {
        size_t n = to_segments(ss, segs);
        CHECK(n == 1 && segs[0].pulses == 45 && segs[0].freq == 1);
        const int64_t free_run = ambit_trace_estimate_ms_for(segs, n, AMBIT_TRACE_ENGINE_FREE_RUN);
        const int64_t paced    = ambit_trace_estimate_ms_for(segs, n, AMBIT_TRACE_ENGINE_PACED);
        CHECK(free_run == 45300); /* 45 s + 300 ms */
        CHECK(paced    == 45600); /* 45 s + 500 ms setup + 100 ms */
        /* Each estimate bounds its own engine's measured run — the paced one
         * with the guard that 90 % of the free-run estimate (40.8 s) lacked. */
        CHECK(free_run > SS_FREE_RUN_MEASURED_MS);
        CHECK(paced    > SS_PACED_MEASURED_MS);
        CHECK(free_run * 90 / 100 < SS_PACED_MEASURED_MS); /* the 2.5.2 defect, pinned */
        CHECK(ambit_trace_estimate_ms(segs, n) == free_run);
        printf("SS   free-run %lld ms  paced %lld ms\n", (long long)free_run, (long long)paced);
    }

    const sched_protocol_t *mpf = protocol(p, "MPF");
    CHECK(mpf != NULL);
    if (mpf != NULL) {
        size_t n = to_segments(mpf, segs);
        CHECK(n == 6);
        const int64_t free_run = ambit_trace_estimate_ms_for(segs, n, AMBIT_TRACE_ENGINE_FREE_RUN);
        const int64_t paced    = ambit_trace_estimate_ms_for(segs, n, AMBIT_TRACE_ENGINE_PACED);
        CHECK(free_run == 10800); /* 9.0 s + 6 × 300 ms */
        CHECK(paced    == 10100); /* 9.0 s + 500 ms + 6 × 100 ms */
        CHECK(ambit_trace_estimate_ms(segs, n) == free_run);
        printf("MPF  free-run %lld ms  paced %lld ms\n", (long long)free_run, (long long)paced);
    }
}

static void test_catalog_bound(const sched_program_t *p, const char *path)
{
    ambit_trace_segment_t segs[SCHED_SPEC_MAX_SEGMENTS];
    /* A spectrum-only schedule (legacy_1hz_spec.yaml) has no protocols. */
    for (int i = 0; i < p->protocol_count; i++) {
        const sched_protocol_t *proto = &p->protocols[i];
        const char *name = sched_pool_str(p, proto->name_off);
        size_t n = to_segments(proto, segs);
        const int64_t free_run = ambit_trace_estimate_ms_for(segs, n, AMBIT_TRACE_ENGINE_FREE_RUN);
        const int64_t paced    = ambit_trace_estimate_ms_for(segs, n, AMBIT_TRACE_ENGINE_PACED);
        CHECK(free_run > 0 && paced > 0);
        CHECK(free_run < MAINTENANCE_SETTLE_MS);
        CHECK(paced < MAINTENANCE_SETTLE_MS);
        /* The paced setup term never outweighs the per-segment slack it
         * replaces by more than it should: both estimates stay within one
         * second of each other for every shipped protocol. */
        CHECK(paced - free_run < 1000 && free_run - paced < 5000);
        printf("%-28s %-12s free-run %6lld ms  paced %6lld ms\n",
               path, name ? name : "?", (long long)free_run, (long long)paced);
    }
}

static void test_edge_cases(void)
{
    ambit_trace_segment_t one = { .type = 2, .pulses = 2, .freq = 0, .subsampling = 1 };
    CHECK(ambit_trace_estimate_ms_for(NULL, 1, AMBIT_TRACE_ENGINE_PACED) == 0);
    CHECK(ambit_trace_estimate_ms_for(&one, 0, AMBIT_TRACE_ENGINE_PACED) == 0);
    CHECK(ambit_trace_estimate_ms_for(&one, 0, AMBIT_TRACE_ENGINE_FREE_RUN) == 0);
    CHECK(ambit_trace_estimate_ms(NULL, 1) == 0);
    /* freq 0 is guarded as 1 Hz (the established behaviour; the compiler
     * rejects it anyway) on both engines. */
    CHECK(ambit_trace_estimate_ms_for(&one, 1, AMBIT_TRACE_ENGINE_FREE_RUN) == 2300);
    CHECK(ambit_trace_estimate_ms_for(&one, 1, AMBIT_TRACE_ENGINE_PACED) == 2600);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s schedule/default.yaml [more catalog .yaml ...]\n", argv[0]);
        return 2;
    }
    static sched_program_t prog;
    for (int a = 1; a < argc; a++) {
        size_t len = 0;
        char *text = read_file(argv[a], &len);
        if (text == NULL) {
            printf("FAIL: cannot read %s\n", argv[a]);
            s_fails++;
            continue;
        }
        char err[160] = "";
        memset(&prog, 0, sizeof prog);
        esp_err_t rc = sched_compile_text(text, len, &prog, err, sizeof err);
        free(text);
        if (rc != ESP_OK) {
            printf("FAIL: %s does not compile: %s\n", argv[a], err);
            s_fails++;
            continue;
        }
        if (a == 1) test_default_schedule(&prog);
        test_catalog_bound(&prog, argv[a]);
    }
    test_engine_selection();
    test_edge_cases();

    if (s_fails) {
        printf("AMBIT_TRACE_ESTIMATE_HOST_FAILED %d of %d checks\n", s_fails, s_checks);
        return 1;
    }
    printf("AMBIT_TRACE_ESTIMATE_HOST_OK %d checks\n", s_checks);
    return 0;
}
