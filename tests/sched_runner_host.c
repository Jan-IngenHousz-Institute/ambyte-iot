/*
 * sched_runner_host.c — host unit runner for the parts of sched_runner that
 * are deliberately pure (tests/test_sched_runner.py compiles it; pattern:
 * tests/sched_spec_host.c):
 *
 *   - the failure-streak log throttle (sched_runner_priv.h, inline): first
 *     failure, every 300th, recovery — the flood protection the design owes
 *     the historical 536 K-line incident;
 *   - the pure lifecycle transition model used under the firmware portMUX,
 *     including stop-success → immediate start and stale-generation rejection;
 *   - the db/store-event JSON writer (sched_runner_json.c), including the
 *     exact path that reboot-looped hardware: a store-event whose kind is
 *     absent must omit the member, never dereference NULL (T3 review
 *     blocker 5; the compiler now also REQUIRES kind, tested in
 *     tests/sched_spec_host.c + test_sched_spec.py).
 *
 * Semaphore timing remains firmware-only and is covered by the hardware gate;
 * the state/generation rules themselves are the same inline functions tested
 * here and called under the firmware lock.
 *
 * Prints "SCHED_RUNNER_HOST_OK <checks>" and exits 0 when all checks pass.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sched_spec.h"
#include "sched_runner_priv.h"

static int s_checks, s_fails;
#define CHECK(cond)                                                     \
    do {                                                                \
        s_checks++;                                                     \
        if (!(cond)) {                                                  \
            s_fails++;                                                  \
            printf("FAIL %d: %s\n", __LINE__, #cond);                   \
        }                                                               \
    } while (0)

/* ── failure-streak throttle ─────────────────────────────────────────── */

static void test_fail_throttle(void)
{
    CHECK(sched_fail_log_decision(true, 1) == SCHED_FAIL_LOG_FAILURE);
    CHECK(sched_fail_log_decision(true, 2) == SCHED_FAIL_LOG_NONE);
    CHECK(sched_fail_log_decision(true, 299) == SCHED_FAIL_LOG_NONE);
    CHECK(sched_fail_log_decision(true, 300) == SCHED_FAIL_LOG_FAILURE);
    CHECK(sched_fail_log_decision(true, 301) == SCHED_FAIL_LOG_NONE);
    CHECK(sched_fail_log_decision(false, 0) == SCHED_FAIL_LOG_NONE);
    CHECK(sched_fail_log_decision(false, 1) == SCHED_FAIL_LOG_RECOVERY);
    CHECK(sched_fail_log_decision(false, 700) == SCHED_FAIL_LOG_RECOVERY);

    /* A 1,000-failure streak logs exactly 4 lines: 1, 300, 600, 900. */
    int logs = 0;
    for (uint32_t streak = 1; streak <= 1000; streak++) {
        if (sched_fail_log_decision(true, streak) == SCHED_FAIL_LOG_FAILURE) {
            logs++;
        }
    }
    CHECK(logs == 4);
}

/* ── lifecycle generation/order model ───────────────────────────────── */

static void test_lifecycle_immediate_restart(void)
{
    sched_lifecycle_t life = SCHED_LIFECYCLE_INITIALIZER;
    uint32_t first = 0, second = 0;
    bool stop_at_publish = false;

    CHECK(sched_lifecycle_begin_start(&life, &first));
    CHECK(first != 0 && life.state == SCHED_LIFE_STARTING);
    CHECK(sched_lifecycle_publish_start(&life, first, &stop_at_publish));
    CHECK(!stop_at_publish && life.state == SCHED_LIFE_RUNNING);

    CHECK(sched_lifecycle_request_stop(&life) == first);
    CHECK(life.state == SCHED_LIFE_STOPPING);

    /* Successful completion publishes STOPPED and the completed generation
     * together. A caller released at this exact boundary can start again
     * immediately; no transient STOPPING state remains observable. */
    CHECK(sched_lifecycle_complete(&life, first));
    CHECK(sched_lifecycle_generation_complete(&life, first));
    CHECK(life.state == SCHED_LIFE_STOPPED);
    CHECK(sched_lifecycle_begin_start(&life, &second));
    CHECK(second != first && life.state == SCHED_LIFE_STARTING);

    /* A late wake/completion from the first run cannot complete or stop the
     * newly claimed generation. */
    CHECK(!sched_lifecycle_complete(&life, first));
    CHECK(!sched_lifecycle_generation_complete(&life, second));
    CHECK(life.state == SCHED_LIFE_STARTING);

    sched_lifecycle_t during_start = SCHED_LIFECYCLE_INITIALIZER;
    uint32_t third = 0;
    CHECK(sched_lifecycle_begin_start(&during_start, &third));
    CHECK(sched_lifecycle_request_stop(&during_start) == third);
    CHECK(during_start.state == SCHED_LIFE_STARTING);
    CHECK(sched_lifecycle_publish_start(&during_start, third, &stop_at_publish));
    CHECK(stop_at_publish && during_start.state == SCHED_LIFE_STOPPING);
    CHECK(sched_lifecycle_complete(&during_start, third));
}

static void test_lifecycle_retains_completion_proof(void)
{
    sched_lifecycle_t life = SCHED_LIFECYCLE_INITIALIZER;
    uint32_t first = 0, second = 0;

    CHECK(sched_lifecycle_begin_start(&life, &first));
    CHECK(sched_lifecycle_publish_start(&life, first, NULL));
    CHECK(sched_lifecycle_complete(&life, first));

    CHECK(sched_lifecycle_begin_start(&life, &second));
    CHECK(sched_lifecycle_publish_start(&life, second, NULL));
    CHECK(sched_lifecycle_complete(&life, second));

    /* A delayed waiter for N must retain proof after N+1 completes. */
    CHECK(sched_lifecycle_generation_complete(&life, first));
    CHECK(sched_lifecycle_generation_complete(&life, second));
    CHECK(!sched_lifecycle_generation_complete(&life, second + 1));
}

static void test_lifecycle_completion_wrap(void)
{
    /* Zero is reserved, so the generation after UINT32_MAX is 1. */
    sched_lifecycle_t life = {
        SCHED_LIFE_STOPPED, UINT32_MAX - 1, UINT32_MAX - 1, false
    };
    uint32_t before_wrap = 0, after_wrap = 0;

    CHECK(sched_lifecycle_begin_start(&life, &before_wrap));
    CHECK(before_wrap == UINT32_MAX);
    CHECK(sched_lifecycle_publish_start(&life, before_wrap, NULL));
    CHECK(sched_lifecycle_complete(&life, before_wrap));

    CHECK(sched_lifecycle_begin_start(&life, &after_wrap));
    CHECK(after_wrap == 1);
    CHECK(sched_lifecycle_publish_start(&life, after_wrap, NULL));
    /* UINT32_MAX completion is stale for the newer generation 1. */
    CHECK(!sched_lifecycle_generation_complete(&life, after_wrap));
    CHECK(sched_lifecycle_complete(&life, after_wrap));
    /* Completing generation 1 retains proof for UINT32_MAX across wrap. */
    CHECK(sched_lifecycle_generation_complete(&life, before_wrap));
    CHECK(sched_lifecycle_generation_complete(&life, after_wrap));
    CHECK(!sched_lifecycle_generation_complete(&life, 2));
}

/* ── JSON writer primitives ──────────────────────────────────────────── */

static void test_jw_str(void)
{
    char buf[64];
    sched_jw_t w = { buf, sizeof buf, 0, false };

    /* NULL writes "" — the runner must never dereference it. */
    sched_jw_str(&w, NULL);
    CHECK(!w.overflow && w.len == 2 && memcmp(buf, "\"\"", 3) == 0);

    w.len = 0;
    sched_jw_str(&w, "plain");
    CHECK(!w.overflow && strcmp(buf, "\"plain\"") == 0);

    /* quote and backslash escaping */
    w.len = 0;
    sched_jw_str(&w, "a\"b\\c");
    CHECK(!w.overflow && strcmp(buf, "\"a\\\"b\\\\c\"") == 0);

    /* overflow: sets the flag, never writes past cap, keeps the buffer
     * NUL-terminated within cap */
    char tiny[4];
    sched_jw_t t = { tiny, sizeof tiny, 0, false };
    sched_jw_str(&t, "abcdef");
    CHECK(t.overflow && t.len >= sizeof tiny && tiny[sizeof tiny - 1] == '\0');
}

static void test_job_stat_placeholders(void)
{
    char buf[32];
    sched_runner_act_ctx_t ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.skipped = 1234;
    ctx.skipped_saturated = true;

    sched_jw_t w = { buf, sizeof buf, 0, false };
    CHECK(sched_jw_job_placeholder(&w, "$job.skipped", &ctx));
    CHECK(!w.overflow && strcmp(buf, "1234") == 0);

    w.len = 0;
    CHECK(sched_jw_job_placeholder(&w, "$job.skipped_saturated", &ctx));
    CHECK(!w.overflow && strcmp(buf, "true") == 0);

    ctx.skipped_saturated = false;
    w.len = 0;
    CHECK(sched_jw_job_placeholder(&w, "$job.skipped_saturated", &ctx));
    CHECK(!w.overflow && strcmp(buf, "false") == 0);

    w.len = 0;
    CHECK(!sched_jw_job_placeholder(&w, "$deployment", &ctx));
    CHECK(w.len == 0);
}

/* ── sched_build_map_json on a compiled store-event step ─────────────── */

static int s_stub_calls;
static char s_stub_last[64];

static void stub_placeholder(sched_jw_t *w, const char *ph,
                             const sched_runner_act_ctx_t *ctx)
{
    (void)ctx;
    s_stub_calls++;
    snprintf(s_stub_last, sizeof s_stub_last, "%s", ph);
    sched_jw_raw(w, "\"resolved:%s\"", ph);
}

static sched_program_t *compile_or_fail(const char *yaml, int line)
{
    sched_program_t *p = calloc(1, sizeof(*p));
    char err[256];
    if (sched_compile_text(yaml, strlen(yaml), p, err, sizeof(err)) != ESP_OK) {
        printf("FAIL %d: compile failed: %s\n", line, err);
        s_checks++;
        s_fails++;
        free(p);
        return NULL;
    }
    s_checks++;
    return p;
}
#define COMPILE_OK(yaml) compile_or_fail(yaml, __LINE__)

static const sched_step_t *find_step(const sched_program_t *p,
                                     const char *action_name)
{
    for (int j = 0; j < p->job_count; j++) {
        for (int s = 0; s < p->jobs[j].step_count; s++) {
            if (strcmp(p->jobs[j].steps[s].action->name, action_name) == 0) {
                return &p->jobs[j].steps[s];
            }
        }
    }
    return NULL;
}

static const char *k_full_yaml =
    "schema: jii.ambyte-schedule/v1-draft\n"
    "name: t\n"
    "jobs:\n"
    "  j:\n"
    "    schedule:\n"
    "      cron: \"*/5 * * * *\"\n"
    "    steps:\n"
    "      - uses: db/store-event\n"
    "        with:\n"
    "          kind: boot\n"
    "          data:\n"
    "            n: 42\n"
    "            f: 2.5\n"
    "            ok: true\n"
    "            s: \"hi \\\"x\\\"\"\n"
    "            ph: $deployment\n"
    "          metadata:\n"
    "            fw: \"1.3.0\"\n";

static void test_build_map_json(void)
{
    sched_runner_act_ctx_t ctx;
    memset(&ctx, 0, sizeof ctx);

    sched_program_t *p = COMPILE_OK(k_full_yaml);
    if (p == NULL) return;
    const sched_step_t *st = find_step(p, "db/store-event");
    CHECK(st != NULL);
    if (st == NULL) { free(p); return; }

    char buf[512];

    /* data map with kind stamped first (the on-device call shape) */
    CHECK(sched_build_map_json(buf, sizeof buf, "data", "kind", "boot",
                               st, p, &ctx, stub_placeholder));
    CHECK(strcmp(buf, "{\"kind\":\"boot\",\"n\":42,\"f\":2.5,\"ok\":true,"
                      "\"s\":\"hi \\\"x\\\"\",\"ph\":\"resolved:$deployment\"}") == 0);
    CHECK(s_stub_calls == 1 && strcmp(s_stub_last, "$deployment") == 0);

    /* metadata map, no stamped member */
    CHECK(sched_build_map_json(buf, sizeof buf, "metadata", NULL, NULL,
                               st, p, &ctx, stub_placeholder));
    CHECK(strcmp(buf, "{\"fw\":\"1.3.0\"}") == 0);

    /* T3 review blocker 5 regression: extra_val NULL (absent kind) must OMIT
     * the member, never crash — this is the path that reboot-looped hardware
     * when a store-event ran with no `with:` map at all. */
    CHECK(sched_build_map_json(buf, sizeof buf, "data", "kind", NULL,
                               st, p, &ctx, stub_placeholder));
    CHECK(strncmp(buf, "{\"n\":42", 7) == 0);

    /* a store-event with kind only: absent data/metadata maps serialize as
     * empty objects (absent optional inputs are tolerated end to end) */
    sched_program_t *q = COMPILE_OK(
        "schema: jii.ambyte-schedule/v1-draft\n"
        "jobs:\n"
        "  j:\n"
        "    schedule:\n"
        "      cron: \"*/5 * * * *\"\n"
        "    steps:\n"
        "      - uses: db/store-event\n"
        "        with:\n"
        "          kind: k\n");
    if (q != NULL) {
        const sched_step_t *st2 = find_step(q, "db/store-event");
        CHECK(st2 != NULL);
        if (st2 != NULL) {
            CHECK(sched_build_map_json(buf, sizeof buf, "data", "kind", "k",
                                       st2, q, &ctx, stub_placeholder));
            CHECK(strcmp(buf, "{\"kind\":\"k\"}") == 0);
            CHECK(sched_build_map_json(buf, sizeof buf, "metadata", NULL, NULL,
                                       st2, q, &ctx, stub_placeholder));
            CHECK(strcmp(buf, "{}") == 0);
        }
        free(q);
    }

    /* overflow fails the step instead of storing a torn object */
    char tiny[12];
    CHECK(!sched_build_map_json(tiny, sizeof tiny, "data", "kind", "boot",
                                st, p, &ctx, stub_placeholder));

    free(p);
}

/* ── brownout protection: shedding (sched_runner_shed.c) ─────────────── */

static sched_program_t *compile_file(const char *path, int line)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        printf("FAIL %d: open %s\n", line, path);
        s_checks++;
        s_fails++;
        return NULL;
    }
    static char buf[SCHED_YAML_MAX_FILE_BYTES + 1];
    const size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';
    return compile_or_fail(buf, line);
}

static const sched_job_t *job_named(const sched_program_t *p, const char *name)
{
    const sched_job_t *j = sched_find_job(p, name);
    if (j == NULL) printf("missing job %s\n", name);
    return j;
}

/* (no_flash, hold) decisions for one job. */
#define SHED(p, name, nf, hold)                                                          \
    do {                                                                                 \
        const sched_job_t *j_ = job_named(p, name);                                      \
        CHECK(j_ != NULL && sched_job_shed(p, j_, false, false) == SCHED_SHED_NONE);      \
        CHECK(j_ != NULL && sched_job_shed(p, j_, true, false) == (nf));                  \
        CHECK(j_ != NULL && sched_job_shed(p, j_, true, true) == (hold));                 \
    } while (0)

static const char *k_shed_yaml =
    "schema: jii.ambyte-schedule/v1-draft\n"
    "name: shed\n"
    "protocols:\n"
    "  DARK:\n"
    "    - pulses: 10\n"
    "      freq: 10\n"
    "      actinic: 0\n"
    "  LIT:\n"
    "    - pulses: 10\n"
    "      freq: 10\n"
    "      actinic: 0\n"
    "    - pulses: 10\n"
    "      freq: 10\n"
    "      actinic: 500\n"
    "  FLASH:\n"
    "    - pulses: 10\n"
    "      freq: 100\n"
    "      actinic: -250\n"
    "jobs:\n"
    "  dark_named_mpf:\n"          /* a name says nothing: content decides */
    "    schedule:\n"
    "      cron: \"* * * * *\"\n"
    "    steps:\n"
    "      - uses: ambit/trace\n"
    "        with:\n"
    "          protocol: DARK\n"
    "          tag: MPF\n"
    "  lit:\n"
    "    schedule:\n"
    "      cron: \"* * * * *\"\n"
    "    steps:\n"
    "      - uses: ambit/trace\n"
    "        with:\n"
    "          protocol: LIT\n"
    "  flash:\n"
    "    schedule:\n"
    "      cron: \"* * * * *\"\n"
    "    steps:\n"
    "      - uses: ambit/trace\n"
    "        with:\n"
    "          protocol: FLASH\n"
    "  actinic_off:\n"
    "    schedule:\n"
    "      cron: \"* * * * *\"\n"
    "    steps:\n"
    "      - uses: ambit/actinic\n"
    "        with:\n"
    "          level: 0\n"
    "          duration: 1s\n"
    "  actinic_par:\n"
    "    schedule:\n"
    "      cron: \"* * * * *\"\n"
    "    steps:\n"
    "      - uses: ambit/actinic\n"
    "        with:\n"
    "          level: 200\n"
    "          duration: 1s\n"
    "  actinic_dac:\n"
    "    schedule:\n"
    "      cron: \"* * * * *\"\n"
    "    steps:\n"
    "      - uses: ambit/actinic\n"
    "        with:\n"
    "          level: -40\n"
    "          duration: 1s\n"
    "  leaf:\n"
    "    schedule:\n"
    "      cron: \"* * * * *\"\n"
    "    steps:\n"
    "      - uses: ambit/leaf-temp\n"
    "  mixed:\n"                    /* store-event then a flash: the whole job goes */
    "    schedule:\n"
    "      cron: \"* * * * *\"\n"
    "    steps:\n"
    "      - uses: db/store-event\n"
    "        with:\n"
    "          kind: before\n"
    "      - uses: ambit/trace\n"
    "        with:\n"
    "          protocol: FLASH\n"
    "  bookkeeping:\n"
    "    schedule:\n"
    "      cron: \"* * * * *\"\n"
    "    steps:\n"
    "      - uses: device/log\n"
    "        with:\n"
    "          message: hi\n"
    "      - uses: db/store-event\n"
    "        with:\n"
    "          kind: k\n"
    "      - uses: device/sleep\n"
    "        with:\n"
    "          duration: 1s\n";

static void test_shed_by_content_and_state(void)
{
    /* The shipped default schedule: SS (actinic 0 everywhere) and spectra keep
     * running in no_flash; MPF and the sunrise/sunset MPF are shed; hold sheds
     * every ambit/ job but never the status report. */
    sched_program_t *d = compile_file("components/sched_runner/default.yaml", __LINE__);
    if (d != NULL) {
        SHED(d, "steady_state",     SCHED_SHED_NONE,  SCHED_SHED_AMBIT);
        SHED(d, "spectra",          SCHED_SHED_NONE,  SCHED_SHED_AMBIT);
        SHED(d, "saturating_flash", SCHED_SHED_FLASH, SCHED_SHED_FLASH);
        SHED(d, "dark_edge",        SCHED_SHED_FLASH, SCHED_SHED_FLASH);
        SHED(d, "health",           SCHED_SHED_NONE,  SCHED_SHED_NONE);
        free(d);
    }
    /* The released qE schedule: the qe job lights the LED and flashes. */
    sched_program_t *q = compile_file("schedule/qe_multichannel.yaml", __LINE__);
    if (q != NULL) {
        SHED(q, "qe",               SCHED_SHED_FLASH, SCHED_SHED_FLASH);
        SHED(q, "steady_state",     SCHED_SHED_NONE,  SCHED_SHED_AMBIT);
        SHED(q, "spectra",          SCHED_SHED_NONE,  SCHED_SHED_AMBIT);
        SHED(q, "saturating_flash", SCHED_SHED_FLASH, SCHED_SHED_FLASH);
        SHED(q, "health",           SCHED_SHED_NONE,  SCHED_SHED_NONE);
        free(q);
    }
    sched_program_t *p = COMPILE_OK(k_shed_yaml);
    if (p == NULL) return;
    SHED(p, "dark_named_mpf", SCHED_SHED_NONE,  SCHED_SHED_AMBIT);
    SHED(p, "lit",            SCHED_SHED_FLASH, SCHED_SHED_FLASH);
    SHED(p, "flash",          SCHED_SHED_FLASH, SCHED_SHED_FLASH);
    SHED(p, "actinic_off",    SCHED_SHED_NONE,  SCHED_SHED_AMBIT);
    SHED(p, "actinic_par",    SCHED_SHED_FLASH, SCHED_SHED_FLASH);
    SHED(p, "actinic_dac",    SCHED_SHED_FLASH, SCHED_SHED_FLASH);
    SHED(p, "leaf",           SCHED_SHED_NONE,  SCHED_SHED_AMBIT);
    SHED(p, "mixed",          SCHED_SHED_FLASH, SCHED_SHED_FLASH);
    SHED(p, "bookkeeping",    SCHED_SHED_NONE,  SCHED_SHED_NONE);
    CHECK(sched_job_shed(NULL, NULL, true, true) == SCHED_SHED_NONE);
    free(p);
}

/* A shed run is not a failure: it never touches runs / failures /
 * fail_streak, never produces a failure log, and a real failure streak spans
 * an episode of skips unbroken. */
static void test_shed_is_not_a_failure(void)
{
    sched_job_counts_t c;
    memset(&c, 0, sizeof c);
    sched_account_t a = sched_job_account(&c, SCHED_RUN_FAILED);
    CHECK(a.fail_log == SCHED_FAIL_LOG_FAILURE && c.fail_streak == 1 && c.failures == 1);
    int shed_logs = 0;
    for (int i = 0; i < 1000; i++) {
        a = sched_job_account(&c, SCHED_RUN_SHED);
        CHECK(a.fail_log == SCHED_FAIL_LOG_NONE);
        if (a.shed_log) shed_logs++;
    }
    CHECK(shed_logs == 4);                       /* 1, 300, 600, 900 */
    CHECK(c.runs == 0 && c.failures == 1 && c.fail_streak == 1);
    CHECK(c.power_skipped == 1000 && c.shed_streak == 1000);
    a = sched_job_account(&c, SCHED_RUN_FAILED);  /* the streak continues: 2, not 1 */
    CHECK(c.fail_streak == 2 && a.fail_log == SCHED_FAIL_LOG_NONE && a.resumed_after == 1000);
    CHECK(c.shed_streak == 0);
    a = sched_job_account(&c, SCHED_RUN_OK);
    CHECK(a.fail_log == SCHED_FAIL_LOG_RECOVERY && a.fail_streak == 2 && c.runs == 1 && c.fail_streak == 0);
    a = sched_job_account(&c, SCHED_RUN_SHED);
    CHECK(a.shed_log && c.shed_streak == 1 && c.power_skipped == 1001);
    a = sched_job_account(&c, SCHED_RUN_OK);
    CHECK(a.resumed_after == 1 && a.fail_log == SCHED_FAIL_LOG_NONE && c.runs == 2);
}

int main(void)
{
    test_shed_by_content_and_state();
    test_shed_is_not_a_failure();
    test_fail_throttle();
    test_lifecycle_immediate_restart();
    test_lifecycle_retains_completion_proof();
    test_lifecycle_completion_wrap();
    test_jw_str();
    test_job_stat_placeholders();
    test_build_map_json();

    if (s_fails == 0) {
        printf("SCHED_RUNNER_HOST_OK %d checks\n", s_checks);
        return 0;
    }
    printf("SCHED_RUNNER_HOST_FAIL %d/%d failed\n", s_fails, s_checks);
    return 1;
}
