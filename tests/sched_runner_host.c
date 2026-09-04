/*
 * sched_runner_host.c — host unit runner for the parts of sched_runner that
 * are deliberately pure (tests/test_sched_runner.py compiles it; pattern:
 * tests/sched_spec_host.c):
 *
 *   - the failure-streak log throttle (sched_runner_priv.h, inline): first
 *     failure, every 300th, recovery — the flood protection the design owes
 *     legacy_1Hz_spec.lua's 536 K-line incident;
 *   - the db/store-event JSON writer (sched_runner_json.c), including the
 *     exact path that reboot-looped hardware: a store-event whose kind is
 *     absent must omit the member, never dereference NULL (T3 review
 *     blocker 5; the compiler now also REQUIRES kind, tested in
 *     tests/sched_spec_host.c + test_sched_spec.py).
 *
 * The runner task/lifecycle itself is firmware-only (FreeRTOS) and is covered
 * by the hardware gate, not here.
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
    "    on: { every: 5m }\n"
    "    steps:\n"
    "      - uses: db/store-event\n"
    "        with:\n"
    "          kind: boot\n"
    "          data: { n: 42, f: 2.5, ok: true, s: \"hi \\\"x\\\"\", ph: $deployment }\n"
    "          metadata: { fw: \"1.3.0\" }\n";

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
        "    on: { every: 5m }\n"
        "    steps: [ { uses: db/store-event, with: { kind: k } } ]\n");
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

int main(void)
{
    test_fail_throttle();
    test_jw_str();
    test_build_map_json();

    if (s_fails == 0) {
        printf("SCHED_RUNNER_HOST_OK %d checks\n", s_checks);
        return 0;
    }
    printf("SCHED_RUNNER_HOST_FAIL %d/%d failed\n", s_fails, s_checks);
    return 1;
}
