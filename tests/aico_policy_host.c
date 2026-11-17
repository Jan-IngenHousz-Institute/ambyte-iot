/* Host harness: the production AICO re-arm policy
 * (components/mp2731/mp2731_aico.c) driven verbatim by synthetic reads.
 * tests/test_aico.py runs every scenario (exit 0 = pass). */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mp2731_aico.h"

#define CHECK(cond, ...) do { if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d: %s - ", __FILE__, __LINE__, #cond); \
    fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); exit(1); } } while (0)

#define S 1000LL
#define MIN (60 * S)

static int64_t now_ms = 12345;

/* Reads every 5 s for `ms`; returns how many asked for a check, answering
 * each with `r` at the moment it was asked. */
static int run(aico_t *a, int64_t ms, bool veto, aico_result_t r)
{
    int checks = 0;
    for (int64_t end = now_ms + ms; now_ms < end; now_ms += 5 * S) {
        if (aico_step(a, now_ms, veto) == AICO_CHECK) {
            checks++;
            aico_note_result(a, now_ms, r);
        }
    }
    return checks;
}

int main(int argc, char **argv)
{
    CHECK(argc == 2, "usage");
    const char *c = argv[1];
    aico_t a;
    aico_init(&a);
    if (!strcmp(c, "interval")) {
        /* The first read only starts the clock. */
        CHECK(aico_step(&a, now_ms, false) == AICO_NONE && a.started, "first read");
        const int64_t t0 = now_ms;
        CHECK(aico_next_in_s(&a, now_ms) == 3600, "next %u", aico_next_in_s(&a, now_ms));
        now_ms += 5 * S;
        CHECK(run(&a, 60 * MIN - 10 * S, false, AICO_RESULT_RESET) == 0, "nothing inside the hour");
        CHECK(run(&a, 10 * S, false, AICO_RESULT_RESET) == 1, "one at the hour");
        CHECK(a.resets == 1 && a.has_reset && a.last_reset_ms >= t0 + 60 * MIN &&
              a.last_reset_ms < t0 + 60 * MIN + 5 * S, "reset recorded at %lld", (long long)a.last_reset_ms);
        /* A day: never two resets closer than the interval. */
        int64_t last = a.last_reset_ms;
        for (int h = 0; h < 24; h++) {
            CHECK(run(&a, 60 * MIN, false, AICO_RESULT_RESET) == 1, "hour %d", h);
            CHECK(a.last_reset_ms - last >= AICO_RESET_INTERVAL_MS, "spacing %lld",
                  (long long)(a.last_reset_ms - last));
            last = a.last_reset_ms;
        }
        CHECK(a.resets == 25 && a.failed == 0 && a.vetoed == 0, "counts %u", a.resets);
    } else if (!strcmp(c, "off")) {
        /* AICO_EN=0: one REG02 look per interval, nothing written, nothing counted. */
        (void)aico_step(&a, now_ms, false);
        CHECK(run(&a, 5 * 60 * MIN + 10 * S, false, AICO_RESULT_OFF) == 5, "hourly looks");
        CHECK(a.resets == 0 && a.failed == 0 && a.enabled_seen == 0 && !a.has_reset && a.checks == 5,
              "off is not a reset");
    } else if (!strcmp(c, "failed")) {
        (void)aico_step(&a, now_ms, false);
        now_ms += 60 * MIN;
        CHECK(run(&a, 5 * S, false, AICO_RESULT_FAILED) == 1 && a.failed == 1, "first failure");
        CHECK(run(&a, AICO_RETRY_MS - 5 * S, false, AICO_RESULT_FAILED) == 0, "not before the retry delay");
        CHECK(run(&a, 5 * S, false, AICO_RESULT_RESET) == 1 && a.resets == 1, "retried after 5 min");
        CHECK(run(&a, 60 * MIN - 5 * S, false, AICO_RESULT_RESET) == 0, "then a full interval");
        CHECK(a.failed == 1 && a.resets == 1, "a failure is not a reset");
    } else if (!strcmp(c, "veto")) {
        (void)aico_step(&a, now_ms, false);
        now_ms += 60 * MIN;
        CHECK(run(&a, 30 * MIN, true, AICO_RESULT_RESET) == 0 && a.vetoed == 1, "held, counted once");
        CHECK(aico_next_in_s(&a, now_ms) == 0, "due");
        CHECK(run(&a, 5 * S, false, AICO_RESULT_RESET) == 1 && a.resets == 1, "fires once the veto clears");
        CHECK(!a.waiting_veto, "wait closed");
        now_ms += 60 * MIN;
        CHECK(run(&a, 5 * S, true, AICO_RESULT_RESET) == 0 && a.vetoed == 2, "a new wait counts again");
    } else if (!strcmp(c, "operator")) {
        (void)aico_step(&a, now_ms, false);
        now_ms += 50 * MIN;
        /* Switching on restarts the period: AICO learns for a full hour first. */
        aico_note_enabled(&a, now_ms, true);
        CHECK(a.enabled_seen == 1 && aico_next_in_s(&a, now_ms) == 3600, "restart");
        CHECK(run(&a, 60 * MIN - 5 * S, false, AICO_RESULT_RESET) == 0, "no reset before an hour of learning");
        CHECK(run(&a, 10 * S, false, AICO_RESULT_RESET) == 1, "then one");
        /* A manual reset counts and restarts the period too. */
        now_ms += 30 * MIN;
        aico_note_result(&a, now_ms, AICO_RESULT_RESET);
        CHECK(a.resets == 2, "manual counted");
        CHECK(run(&a, 60 * MIN - 5 * S, false, AICO_RESULT_RESET) == 0, "no automatic reset within the hour");
        aico_note_enabled(&a, now_ms, false);
        CHECK(a.enabled_seen == 0, "off noted");
        /* A note before any read starts the policy. */
        aico_t b; aico_init(&b);
        aico_note_enabled(&b, 100, true);
        CHECK(aico_step(&b, 100 + 60 * MIN, false) == AICO_CHECK, "started by the switch");
    } else if (!strcmp(c, "render")) {
        uint8_t regs[24] = {0};
        regs[0x02] = 0xDD; regs[0x14] = 0x0E; regs[0x15] = 0x80;
        char out[AICO_RENDER_CAP];
        int n = aico_render(&a, regs, 0xFFFFFF, out, sizeof out);
        CHECK(n > 0 && (size_t)n == strlen(out), "fresh");
        printf("RENDER_FRESH=%s\n", out);
        n = aico_render(&a, regs, 0, out, sizeof out);
        printf("RENDER_UNSCANNED=%s\n", out);
        CHECK(n > 0, "unscanned");
        n = aico_render(&a, regs, 1u << 0x14, out, sizeof out);
        printf("RENDER_PARTIAL=%s\n", out);
        CHECK(n > 0, "partial");
        a.resets = UINT32_MAX; a.failed = UINT32_MAX; a.has_reset = true; a.last_reset_ms = INT64_MIN;
        regs[0x14] = 0x3F;
        n = aico_render(&a, regs, 0xFFFFFF, out, sizeof out);
        CHECK(n > 0 && (size_t)n < AICO_RENDER_CAP, "worst case fits (%d)", n);
        printf("RENDER_WORST=%s\n", out);
        char tiny[16];
        CHECK(aico_render(&a, regs, 0xFFFFFF, tiny, sizeof tiny) == -1 && tiny[0] == '\0', "never truncated");
        CHECK(aico_render(NULL, regs, 0, out, sizeof out) == -1, "null");
    } else {
        fprintf(stderr, "unknown case %s\n", c); return 2;
    }
    puts("OK");
    return 0;
}
