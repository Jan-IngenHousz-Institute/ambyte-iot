/*
 * sched_spec_host.c — unit runner for the sched_spec component, compiled by
 * tests/test_sched_spec.py (pattern: tests/payload_v3_host.c). Pure host C11;
 * time_sync.c is the only component dependency. DST cases drive the offset
 * from the host libc (TZ=Europe/Amsterdam) — the component itself only ever
 * sees local times / an injected localize, never components/timezone.
 *
 * Prints "SCHED_SPEC_HOST_OK <checks>" and exits 0 when all checks pass.
 */

#define _GNU_SOURCE /* tm_gmtoff, timegm, setenv on glibc */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "sched_spec.h"

static int s_checks, s_fails;
#define CHECK(cond)                                                     \
    do {                                                                \
        s_checks++;                                                     \
        if (!(cond)) {                                                  \
            s_fails++;                                                  \
            printf("FAIL %d: %s\n", __LINE__, #cond);                   \
        }                                                               \
    } while (0)

static sched_program_t *compile_or_fail(const char *yaml, int line)
{
    sched_program_t *p = calloc(1, sizeof(*p));
    char err[256];
    if (sched_compile_text(yaml, strlen(yaml), p, err, sizeof(err)) != ESP_OK) {
        printf("FAIL %d: compile failed: %s\n  yaml: %.80s\n", line, err, yaml);
        s_checks++;
        s_fails++;
        free(p);
        return NULL;
    }
    s_checks++;
    return p;
}
#define COMPILE_OK(yaml) compile_or_fail(yaml, __LINE__)

static void expect_compile_err(const char *yaml, const char *needle, int line)
{
    sched_program_t *p = calloc(1, sizeof(*p));
    char err[256];
    esp_err_t rc = sched_compile_text(yaml, strlen(yaml), p, err, sizeof(err));
    s_checks += 2;
    if (rc == ESP_OK) {
        printf("FAIL %d: expected compile error containing '%s'\n", line, needle);
        s_fails++;
    } else if (strstr(err, needle) == NULL) {
        printf("FAIL %d: error '%s' lacks '%s'\n", line, err, needle);
        s_fails++;
    }
    free(p);
}
#define COMPILE_ERR(yaml, needle) expect_compile_err(yaml, needle, __LINE__)

static bool parse_only_ok(const char *yaml)
{
    sched_yaml_doc_t *doc = NULL;
    char err[256];
    bool ok = sched_yaml_parse(yaml, strlen(yaml), &doc, err, sizeof(err)) == ESP_OK;
    sched_yaml_free(doc);
    return ok;
}

static void parse_err_contains(const char *yaml, const char *needle, int line)
{
    sched_yaml_doc_t *doc = NULL;
    char err[256] = { 0 };
    esp_err_t rc = sched_yaml_parse(yaml, strlen(yaml), &doc, err, sizeof(err));
    s_checks += 2;
    if (rc == ESP_OK) {
        printf("FAIL %d: expected parse error containing '%s'\n", line, needle);
        s_fails++;
    } else if (strstr(err, needle) == NULL) {
        printf("FAIL %d: error '%s' lacks '%s'\n", line, err, needle);
        s_fails++;
    }
    sched_yaml_free(doc);
}
#define PARSE_ERR(yaml, needle) parse_err_contains(yaml, needle, __LINE__)

/* ── parser acceptance corpus ────────────────────────────────────────── */

static void test_parser_acceptance(void)
{
    CHECK(parse_only_ok("a: 1\n"));
    CHECK(parse_only_ok("a: true\nb: false\n"));
    CHECK(parse_only_ok("a: -17\nb: 3.25\nc: -0.5\n"));
    CHECK(parse_only_ok("a: 500ms\nb: 2s\nc: 30m\nd: 1h\n"));
    CHECK(parse_only_ok("a: 08:30\nb: \"23:59\"\n"));
    CHECK(parse_only_ok("a: { k: [0, 1], n: { deep: true } }\n"));
    CHECK(parse_only_ok("a: \"quoted # not a comment\"\nb: 'single'\n"));
    CHECK(parse_only_ok("a: \"esc \\\" quote\"\n"));
    CHECK(parse_only_ok("# just a comment\n\na: 1 # trailing\n"));
    /* block sequence under a key, scalars and inline mappings */
    CHECK(parse_only_ok("channels:\n  - 0\n  - 1\n  - 2\n"));
    CHECK(parse_only_ok("steps:\n  - uses: device/log\n    with: { message: hi }\n"
                        "  - uses: device/sleep\n    with: { duration: 5s }\n"));
    /* scalar seq items incl. plain strings */
    CHECK(parse_only_ok("days: [mon, wed, fri]\n"));
    /* CRLF tolerated */
    CHECK(parse_only_ok("a: 1\r\nb: [x, y]\r\n"));

    /* scalar resolution spot-checks */
    sched_yaml_doc_t *doc = NULL;
    char err[128];
    const char *y = "i: -5\nf: 2.5\nd: 30m\nt: 08:05\ns: hello world\n";
    CHECK(sched_yaml_parse(y, strlen(y), &doc, err, sizeof(err)) == ESP_OK);
    const sched_node_t *root = sched_yaml_root(doc);
    CHECK(root != NULL && root->kind == SCHED_NODE_MAP && root->u.m.count == 5);
    if (root != NULL && root->u.m.count == 5) {
        const sched_node_t *i = root->u.m.pairs[0].value;
        CHECK(i->scal_kind == SCHED_SCAL_INT && i->u.s.i == -5);
        const sched_node_t *f = root->u.m.pairs[1].value;
        CHECK(f->scal_kind == SCHED_SCAL_FLOAT && f->u.s.f > 2.49 && f->u.s.f < 2.51);
        const sched_node_t *d = root->u.m.pairs[2].value;
        CHECK(d->scal_kind == SCHED_SCAL_DURATION_MS && d->u.s.ms == 1800000);
        const sched_node_t *t = root->u.m.pairs[3].value;
        CHECK(t->scal_kind == SCHED_SCAL_HHMM && t->u.s.hh == 8 && t->u.s.mm == 5);
        const sched_node_t *s = root->u.m.pairs[4].value;
        CHECK(s->scal_kind == SCHED_SCAL_STR && strcmp(s->u.s.str, "hello world") == 0);
    }
    sched_yaml_free(doc);
}

static void test_parser_rejections(void)
{
    PARSE_ERR("a:\t1\n", "tab");
    PARSE_ERR("a: &anchor 1\n", "anchors");
    PARSE_ERR("a: *alias\n", "aliases");
    PARSE_ERR("a: !tag 1\n", "tags");
    PARSE_ERR("a: |\n  text\n", "block scalars");
    PARSE_ERR("a: >\n  text\n", "block scalars");
    PARSE_ERR("---\na: 1\n", "multi-document");
    PARSE_ERR("a: yes\n", "true/false");
    PARSE_ERR("a: OFF\n", "true/false");
    PARSE_ERR("a: { k: 1,\n  j: 2 }\n", "multi-line flow");
    PARSE_ERR("a: 1\na: 2\n", "duplicate key");
    PARSE_ERR("a: 25:10\n", "invalid time");
    PARSE_ERR("a: 1\n  b: 2\n", "unexpected indentation");
    PARSE_ERR("a:colonless\n", "space after ':'");
    PARSE_ERR("", "empty");

    /* line > 256 chars */
    static char long_line[300];
    memset(long_line, 'x', sizeof(long_line) - 1);
    long_line[0] = 'a';
    long_line[1] = ':';
    long_line[2] = ' ';
    long_line[sizeof(long_line) - 1] = '\0';
    PARSE_ERR(long_line, "line exceeds");

    /* file > 16 KiB */
    size_t big_len = SCHED_YAML_MAX_FILE_BYTES + 100;
    char *big = malloc(big_len + 1);
    memset(big, 'a', big_len);
    big[big_len] = '\0';
    sched_yaml_doc_t *doc = NULL;
    char err[128];
    CHECK(sched_yaml_parse(big, big_len, &doc, err, sizeof(err)) == ESP_FAIL);
    CHECK(doc == NULL);
    free(big);

    /* node cap: a block sequence with > 512 items */
    {
        size_t cap = 8 + 600 * 8;
        char *many = malloc(cap);
        strcpy(many, "a:\n");
        for (int i = 0; i < 600; i++) strcat(many, "  - 1\n");
        PARSE_ERR(many, "node limit");
        free(many);
    }
    /* string pool cap: > 8 KiB of distinct strings */
    {
        size_t cap = 40 * 260 + 8;
        char *strings = malloc(cap);
        strings[0] = '\0';
        for (int i = 0; i < 40; i++) {
            char line[260];
            snprintf(line, sizeof(line), "k%d: '%.240d'\n", i, i);
            strcat(strings, line);
        }
        PARSE_ERR(strings, "string pool limit");
        free(strings);
    }
    /* depth > 6 */
    PARSE_ERR("a:\n  b:\n    c:\n      d:\n        e:\n          f:\n            g: 1\n",
              "nesting deeper");
}

/* ── compiler: beyond the on-disk fixtures ───────────────────────────── */

static void test_compiler_rules(void)
{
    /* valid trigger shapes */
    sched_program_t *p = COMPILE_OK(
        "schema: jii.ambyte-schedule/v1-draft\n"
        "jobs:\n"
        "  j:\n"
        "    on:\n"
        "      - { every: 5m, phase: 30s }\n"
        "      - { at: \"08:00\" }\n"
        "      - { weekly: { days: [mon, wed], at: 9:30 } }\n"
        "      - { cron: \"0 3 * * sun\" }\n"
        "      - { sun: sunset }\n"
        "      - boot\n"
        "      - dispatch\n"
        "    steps: [ { uses: device/log, with: { message: hi } } ]\n");
    CHECK(p != NULL);
    if (p != NULL) {
        CHECK(p->job_count == 1);
        const sched_job_t *j = &p->jobs[0];
        CHECK(j->trigger_count == 7);
        CHECK(j->triggers[0].u.every.period_ms == 300000);
        CHECK(j->triggers[0].u.every.phase_ms == 30000);
        CHECK(j->triggers[2].u.weekly.days_mask == ((1u << 1) | (1u << 3)));
        CHECK(j->triggers[2].u.weekly.hh == 9 && j->triggers[2].u.weekly.mm == 30);
        CHECK(j->triggers[4].u.sun.event == TIME_SYNC_SUNSET);
        CHECK(j->on_enter == 0); /* ungated: no gate to enter */
        CHECK(j->overlap == SCHED_OVERLAP_SKIP && j->missed == SCHED_MISSED_SKIP);
        free(p);
    }

    /* gate: on_enter defaults true for gated jobs; explicit override works */
    p = COMPILE_OK(
        "schema: jii.ambyte-schedule/v1-draft\n"
        "jobs:\n"
        "  j:\n"
        "    on: { every: 5m }\n"
        "    when: { window: night }\n"
        "    overlap: queue-one\n"
        "    missed: run-once\n"
        "    steps: [ { uses: device/log, with: { message: hi } } ]\n");
    CHECK(p != NULL);
    if (p != NULL) {
        const sched_job_t *j = &p->jobs[0];
        CHECK(j->has_window && j->on_enter == 1);
        CHECK(j->window.hint == SCHED_WIN_NIGHT);
        CHECK(j->window.from.event == TIME_SYNC_SUNSET && j->window.to.event == TIME_SYNC_SUNRISE);
        CHECK(j->overlap == SCHED_OVERLAP_QUEUE_ONE && j->missed == SCHED_MISSED_RUN_ONCE);
        free(p);
    }

    /* protocols: run-level map form with persist/allow_interrupt, and the
     * six-field segment defaults */
    p = COMPILE_OK(
        "schema: jii.ambyte-schedule/v1-draft\n"
        "protocols:\n"
        "  P:\n"
        "    segments:\n"
        "      - { pulses: 10, freq: 5, actinic: -100 }\n"
        "    persist: true\n"
        "    allow_interrupt: true\n"
        "jobs:\n"
        "  j:\n"
        "    on: { every: 5m }\n"
        "    steps: [ { uses: ambit/trace, with: { protocol: P, channels: [0] } } ]\n");
    CHECK(p != NULL);
    if (p != NULL) {
        const sched_protocol_t *proto = &p->protocols[0];
        CHECK(proto->persist == 1 && proto->allow_interrupt == 1);
        const sched_segment_t *s = &proto->segments[0];
        CHECK(s->type == 2 && s->far_red == 0 && s->subsampling == 1);
        CHECK(s->pulses == 10 && s->freq == 5 && s->actinic == -100);
        CHECK(sched_estimate_ms(proto) == 2300); /* 10/5 s + 300 ms */
        /* deadline_margin default materialized as an entry (15000 ms) */
        const sched_step_t *st = &p->jobs[0].steps[0];
        bool have_margin = false, have_hold = false;
        for (int e = 0; e < st->entry_count; e++) {
            const sched_entry_t *en = &p->entries[st->entry_start + e];
            const char *iname = st->action->inputs[en->input_idx].name;
            if (strcmp(iname, "deadline_margin") == 0) {
                have_margin = true;
                CHECK(en->u.i == 15000);
            }
            if (strcmp(iname, "hold_window") == 0) {
                have_hold = true;
                CHECK(en->type == SCHED_VAL_BOOL && en->u.i == 0);
            }
        }
        CHECK(have_margin && have_hold);
        free(p);
    }

    /* channels: absent materializes as n == 0 ("all that answer a ping"),
     * distinct from an explicit restriction list */
    p = COMPILE_OK(
        "schema: jii.ambyte-schedule/v1-draft\n"
        "jobs:\n"
        "  a:\n    on: { every: 5m }\n"
        "    steps: [ { uses: ambit/spectrum } ]\n"
        "  b:\n    on: { every: 5m }\n"
        "    steps: [ { uses: ambit/spectrum, with: { channels: [0, 2] } } ]\n");
    CHECK(p != NULL);
    if (p != NULL) {
        const sched_step_t *a = &p->jobs[0].steps[0];
        CHECK(a->entry_count == 1);
        const sched_entry_t *ea = &p->entries[a->entry_start];
        CHECK(ea->type == SCHED_VAL_CHANNELS && ea->u.chans.n == 0);
        const sched_step_t *b = &p->jobs[1].steps[0];
        CHECK(b->entry_count == 1);
        const sched_entry_t *eb = &p->entries[b->entry_start];
        CHECK(eb->type == SCHED_VAL_CHANNELS && eb->u.chans.n == 2);
        CHECK(eb->u.chans.v[0] == 0 && eb->u.chans.v[1] == 2);
        free(p);
    }

    /* db/store-event: placeholders validated, map entries typed */
    p = COMPILE_OK(
        "schema: jii.ambyte-schedule/v1-draft\n"
        "jobs:\n"
        "  j:\n"
        "    on: { cron: \"0 * * * *\" }\n"
        "    steps: [ { uses: db/store-event, with: { kind: hourly, "
        "data: { runs: $job.runs, note: hello, n: 5, x: 1.5, ok: true } } } ]\n");
    CHECK(p != NULL);
    if (p != NULL) {
        const sched_step_t *st = &p->jobs[0].steps[0];
        CHECK(st->entry_count == 6); /* kind + 5 data entries */
        free(p);
    }

    /* every rule of the plan's list that is cheap to trigger inline */
    COMPILE_ERR("schema: jii.ambyte-schedule/v1-draft\njobs:\n  j:\n"
                "    on: { every: 1m, cron: \"0 * * * *\" }\n"
                "    steps: [ { uses: device/log, with: { message: hi } } ]\n",
                "exactly one");
    COMPILE_ERR("schema: jii.ambyte-schedule/v1-draft\njobs:\n  j:\n"
                "    on: { sun: sunrise, offset: 30m, phase: 1s }\n"
                "    steps: [ { uses: device/log, with: { message: hi } } ]\n",
                "phase");
    COMPILE_ERR("schema: jii.ambyte-schedule/v1-draft\njobs:\n  j:\n"
                "    on: [ boot ]\n"
                "    frobnicate: 1\n"
                "    steps: [ { uses: device/log, with: { message: hi } } ]\n",
                "unknown job key");
    COMPILE_ERR("schema: jii.ambyte-schedule/v1-draft\njobs:\n  j:\n"
                "    on: { every: 5m }\n"
                "    steps: [ { uses: device/log, with: { message: hi }, bogus: 1 } ]\n",
                "unknown step key");
    COMPILE_ERR("schema: jii.ambyte-schedule/v1-draft\njobs:\n  j:\n"
                "    on: { every: 5m }\n"
                "    when: { window: { from: sunrise } }\n"
                "    steps: [ { uses: device/log, with: { message: hi } } ]\n",
                "from: and to:");
    COMPILE_ERR("schema: jii.ambyte-schedule/v1-draft\njobs:\n  j:\n"
                "    on: { every: 5m }\n"
                "    when: { window: { from: noon, to: sunset } }\n"
                "    steps: [ { uses: device/log, with: { message: hi } } ]\n",
                "window edge");
    /* duration vs period passes exactly at the boundary (the shipped SS shape) */
    p = COMPILE_OK(
        "schema: jii.ambyte-schedule/v1-draft\n"
        "protocols:\n  SS:\n    - { pulses: 45, freq: 1, actinic: 0 }\n"
        "jobs:\n  j:\n    on: { every: 1m }\n"
        "    steps: [ { uses: ambit/trace, with: { protocol: SS, channels: [0] } } ]\n");
    CHECK(p != NULL);
    free(p);
}

/* ── cron ────────────────────────────────────────────────────────────── */

static void test_cron_basics(void)
{
    sched_cron_t c;
    char err[128];
    CHECK(sched_cron_parse("*/15 2 1,15 jan,mar mon-fri", &c, err, sizeof(err)) == ESP_OK);
    CHECK(c.min == ((1ULL << 0) | (1ULL << 15) | (1ULL << 30) | (1ULL << 45)));
    CHECK(c.hour == (1U << 2));
    CHECK(c.dom == ((1U << 1) | (1U << 15)));
    CHECK(c.month == ((1U << 1) | (1U << 3)));
    CHECK(c.dow == 0x3E); /* mon..fri */
    CHECK(c.dom_restricted && c.dow_restricted);

    CHECK(sched_cron_parse("0 9 * * *", &c, err, sizeof(err)) == ESP_OK);
    CHECK(!c.dom_restricted && !c.dow_restricted);
    sched_tm_like_t t = { 2026, 9, 4, 9, 0, 5 }; /* Friday */
    CHECK(sched_cron_matches(&c, &t));
    t.min = 1;
    CHECK(!sched_cron_matches(&c, &t));

    /* vixie OR rule: "0 0 13 * fri" fires on the 13th AND every Friday */
    CHECK(sched_cron_parse("0 0 13 * fri", &c, err, sizeof(err)) == ESP_OK);
    sched_tm_like_t fri = { 2026, 9, 4, 0, 0, 5 };   /* Friday, not 13th */
    sched_tm_like_t thirteenth = { 2026, 10, 13, 0, 0, 2 }; /* Tuesday 13th */
    sched_tm_like_t other = { 2026, 10, 14, 0, 0, 3 };
    CHECK(sched_cron_matches(&c, &fri));
    CHECK(sched_cron_matches(&c, &thirteenth));
    CHECK(!sched_cron_matches(&c, &other));

    /* AND when only one of dom/dow restricted */
    CHECK(sched_cron_parse("0 0 13 * *", &c, err, sizeof(err)) == ESP_OK);
    CHECK(!sched_cron_matches(&c, &fri));
    CHECK(sched_cron_matches(&c, &thirteenth));

    /* dow 7 folds to Sunday */
    CHECK(sched_cron_parse("0 0 * * 7", &c, err, sizeof(err)) == ESP_OK);
    CHECK(c.dow == 1); /* bit 0 */
    /* ranges across the 7 boundary: 5-7 = fri,sat,sun */
    CHECK(sched_cron_parse("0 0 * * 5-7", &c, err, sizeof(err)) == ESP_OK);
    CHECK((c.dow & ((1u << 5) | (1u << 6) | 1u)) == ((1u << 5) | (1u << 6) | 1u));

    /* rejected extensions and shapes */
    CHECK(sched_cron_parse("0 0 L * *", &c, err, sizeof(err)) == ESP_FAIL);
    CHECK(sched_cron_parse("0 0 * * 5#2", &c, err, sizeof(err)) == ESP_FAIL);
    CHECK(sched_cron_parse("@daily", &c, err, sizeof(err)) == ESP_FAIL);
    CHECK(sched_cron_parse("0 0 * *", &c, err, sizeof(err)) == ESP_FAIL);
    CHECK(sched_cron_parse("0 0 * * * extra", &c, err, sizeof(err)) == ESP_FAIL);
    CHECK(sched_cron_parse("0 25 * * *", &c, err, sizeof(err)) == ESP_FAIL);
    CHECK(sched_cron_parse("0/0 * * * *", &c, err, sizeof(err)) == ESP_FAIL);

    /* next-fire table (linear local frame) */
    int64_t out;
    CHECK(sched_cron_parse("0 9 * * *", &c, err, sizeof(err)) == ESP_OK);
    int64_t after = time_sync_make(2026, 3, 28, 12, 0, 0);
    CHECK(sched_cron_next(&c, after, &out) == ESP_OK);
    CHECK(out == time_sync_make(2026, 3, 29, 9, 0, 0));
    /* strictly after: same-minute input rolls to the next day */
    CHECK(sched_cron_next(&c, time_sync_make(2026, 3, 29, 9, 0, 0), &out) == ESP_OK);
    CHECK(out == time_sync_make(2026, 3, 30, 9, 0, 0));
    /* minute-step search: star/7 from xx:00 → xx:07 */
    CHECK(sched_cron_parse("*/7 * * * *", &c, err, sizeof(err)) == ESP_OK);
    CHECK(sched_cron_next(&c, time_sync_make(2026, 6, 1, 0, 0, 0), &out) == ESP_OK);
    CHECK(out == time_sync_make(2026, 6, 1, 0, 7, 0));
    /* month name + bounded search: "0 0 29 2 *" from 2027 → 2028 (leap) */
    CHECK(sched_cron_parse("0 0 29 feb *", &c, err, sizeof(err)) == ESP_OK);
    CHECK(sched_cron_next(&c, time_sync_make(2027, 3, 1, 0, 0, 0), &out) == ESP_OK);
    CHECK(out == time_sync_make(2028, 2, 29, 0, 0, 0));
    /* beyond 366 days: from 2026-03-01, 2027-02-29 doesn't exist → not found */
    CHECK(sched_cron_next(&c, time_sync_make(2026, 3, 1, 0, 0, 0), &out) == ESP_ERR_NOT_FOUND);
}

/* libc wall-clock ground truth around Europe/Amsterdam DST transitions. */
static int count_wall_matches(const sched_cron_t *c, time_t from_utc, time_t to_utc,
                              int want_y, int want_m, int want_d)
{
    int n = 0;
    for (time_t u = from_utc; u < to_utc; u += 60) {
        struct tm tmv;
        localtime_r(&u, &tmv);
        int64_t frame = (int64_t)u + (int64_t)tmv.tm_gmtoff;
        int y, mo, d, h, mi, wd;
        time_sync_localtime(frame, &y, &mo, &d, &h, &mi, NULL, &wd);
        if (y != want_y || mo != want_m || d != want_d) continue;
        sched_tm_like_t t = { y, mo, d, h, mi, wd };
        if (sched_cron_matches(c, &t)) n++;
    }
    return n;
}

static void test_cron_dst(void)
{
    setenv("TZ", "Europe/Amsterdam", 1);
    tzset();
    sched_cron_t c;
    char err[128];
    CHECK(sched_cron_parse("30 2 * * *", &c, err, sizeof(err)) == ESP_OK);

    /* spring forward 2026-03-29: 02:30 never appears on the wall */
    struct tm day = { 0 };
    day.tm_year = 126; day.tm_mon = 2; day.tm_mday = 28; /* 2026-03-28 UTC */
    time_t t0 = timegm(&day);
    CHECK(count_wall_matches(&c, t0, t0 + 3 * 86400, 2026, 3, 28) == 1);
    CHECK(count_wall_matches(&c, t0, t0 + 3 * 86400, 2026, 3, 29) == 0);
    CHECK(count_wall_matches(&c, t0, t0 + 3 * 86400, 2026, 3, 30) == 1);

    /* fall back 2026-10-25: 02:30 appears twice on the wall (CEST and CET) */
    day.tm_year = 126; day.tm_mon = 9; day.tm_mday = 24;
    t0 = timegm(&day);
    CHECK(count_wall_matches(&c, t0, t0 + 3 * 86400, 2026, 10, 24) == 1);
    CHECK(count_wall_matches(&c, t0, t0 + 3 * 86400, 2026, 10, 25) == 2);
    CHECK(count_wall_matches(&c, t0, t0 + 3 * 86400, 2026, 10, 26) == 1);

    /* the due model de-duplicates the double wall minute and skips the
     * nonexistent one — see test_due_dst */
}

/* ── windows ─────────────────────────────────────────────────────────── */

static sched_window_t mk_window_day(void)
{
    sched_window_t w;
    memset(&w, 0, sizeof(w));
    w.hint = SCHED_WIN_DAY;
    w.unresolved = SCHED_UNRESOLVED_SKIP;
    w.from.kind = SCHED_EDGE_SUN; w.from.event = TIME_SYNC_SUNRISE;
    w.to.kind = SCHED_EDGE_SUN;   w.to.event = TIME_SYNC_SUNSET;
    return w;
}

static void test_windows(void)
{
    /* NL midsummer: sun-up roughly 05:17–22:03 CEST */
    time_sync_set_location(52.173, 5.819, 0);
    time_sync_set_utc_offset_seconds(7200);
    int64_t noon = time_sync_make(2026, 6, 21, 12, 0, 0);
    int64_t night = time_sync_make(2026, 6, 21, 3, 0, 0);

    sched_window_t day_w = mk_window_day();
    CHECK(sched_window_state(&day_w, noon) == SCHED_WINDOW_OPEN);
    CHECK(sched_window_state(&day_w, night) == SCHED_WINDOW_CLOSED);

    /* explicit clock window wrapping midnight */
    sched_window_t wrap;
    memset(&wrap, 0, sizeof(wrap));
    wrap.hint = SCHED_WIN_EXPLICIT;
    wrap.unresolved = SCHED_UNRESOLVED_SKIP;
    wrap.from.kind = SCHED_EDGE_CLOCK; wrap.from.hh = 22; wrap.from.mm = 0;
    wrap.to.kind = SCHED_EDGE_CLOCK;   wrap.to.hh = 2;   wrap.to.mm = 0;
    CHECK(sched_window_state(&wrap, time_sync_make(2026, 6, 21, 23, 0, 0)) == SCHED_WINDOW_OPEN);
    CHECK(sched_window_state(&wrap, time_sync_make(2026, 6, 21, 1, 0, 0)) == SCHED_WINDOW_OPEN);
    CHECK(sched_window_state(&wrap, time_sync_make(2026, 6, 21, 12, 0, 0)) == SCHED_WINDOW_CLOSED);
    CHECK(sched_window_state(&wrap, time_sync_make(2026, 6, 21, 2, 0, 0)) == SCHED_WINDOW_CLOSED);

    /* sun window with offsets */
    sched_window_t sunw;
    memset(&sunw, 0, sizeof(sunw));
    sunw.hint = SCHED_WIN_EXPLICIT;
    sunw.from.kind = SCHED_EDGE_SUN; sunw.from.event = TIME_SYNC_SUNRISE; sunw.from.offset_s = -3600;
    sunw.to.kind = SCHED_EDGE_SUN;   sunw.to.event = TIME_SYNC_SUNSET;  sunw.to.offset_s = 3600;
    CHECK(sched_window_state(&sunw, noon) == SCHED_WINDOW_OPEN);
    CHECK(sched_window_state(&sunw, night) == SCHED_WINDOW_CLOSED);

    /* polar summer at 78° N: no sunrise/sunset at all */
    time_sync_set_location(78.0, 15.6, 0);
    time_sync_set_utc_offset_seconds(7200);
    CHECK(sched_window_state(&sunw, noon) == SCHED_WINDOW_UNRESOLVED);
    /* day keeps is_daytime's polar fallback of true (main.lua continuity) */
    CHECK(sched_window_state(&day_w, noon) == SCHED_WINDOW_OPEN);
    sched_window_t night_w = mk_window_day();
    night_w.hint = SCHED_WIN_NIGHT;
    night_w.from.event = TIME_SYNC_SUNSET;
    night_w.to.event = TIME_SYNC_SUNRISE;
    CHECK(sched_window_state(&night_w, noon) == SCHED_WINDOW_CLOSED);

    /* next_open finds tomorrow's sunrise edge from polar-unresolvable days
     * only when an edge resolves; at 78° N in June it does not */
    int64_t open_t;
    CHECK(!sched_window_next_open(&sunw, noon, &open_t));
    time_sync_set_location(52.173, 5.819, 0);
    time_sync_set_utc_offset_seconds(7200);
    CHECK(sched_window_next_open(&wrap, time_sync_make(2026, 6, 21, 12, 0, 0), &open_t));
    CHECK(open_t == time_sync_make(2026, 6, 21, 22, 0, 0));
}

/* ── due-time model ──────────────────────────────────────────────────── */

static int64_t identity_localize(void *ctx, int64_t utc)
{
    (void)ctx;
    return utc;
}

/* fixed-offset localize driven by libc (Europe/Amsterdam DST) */
static int64_t libc_localize(void *ctx, int64_t utc)
{
    (void)ctx;
    time_t u = (time_t)utc;
    struct tm tmv;
    localtime_r(&u, &tmv);
    return utc + (int64_t)tmv.tm_gmtoff;
}

static sched_due_t *mk_due(const char *yaml, int64_t now, sched_localize_fn fn,
                           sched_program_t **prog_out)
{
    sched_program_t *p = COMPILE_OK(yaml);
    if (p == NULL) return NULL;
    sched_due_t *d = calloc(1, sizeof(*d));
    sched_due_init(d, p, fn, NULL, now);
    *prog_out = p;
    return d;
}

static void test_due_model(void)
{
    sched_program_t *p;
    int64_t t0 = time_sync_make(2026, 6, 21, 0, 0, 0); /* grid-aligned */

    /* late grace: within period → fires late; past period → skipped */
    sched_due_t *d = mk_due(
        "schema: jii.ambyte-schedule/v1-draft\n"
        "jobs:\n  j:\n    on: { every: 1m }\n"
        "    steps: [ { uses: device/log, with: { message: hi } } ]\n",
        t0, identity_localize, &p);
    CHECK(d != NULL);
    if (d != NULL) {
        CHECK(sched_due_poll(d, t0 + 30) == 0);            /* slot at +60 not due */
        CHECK(sched_due_poll(d, t0 + 90) == 1);            /* 30 s late, within grace */
        CHECK(d->jobs[0].skipped == 0);
        CHECK(sched_due_poll(d, t0 + 190) == 1);           /* +120 missed; +180 fires */
        CHECK(d->jobs[0].skipped == 1);
        free(p);
        free(d);
    }

    /* skip counting across a big clock step: 10 m grid, 2 h jump → 11 slots */
    d = mk_due(
        "schema: jii.ambyte-schedule/v1-draft\n"
        "jobs:\n  j:\n    on: { every: 10m }\n"
        "    steps: [ { uses: device/log, with: { message: hi } } ]\n",
        t0, identity_localize, &p);
    if (d != NULL) {
        /* one poll at t0+2h: the 11 slots in (t0, t0+6600] are counted
         * missed, then the grid slot at t0+7200 fires in the same poll */
        CHECK(sched_due_poll(d, t0 + 7200) == 1);
        CHECK(d->jobs[0].skipped == 11);
        free(p);
        free(d);
    }

    /* missed: run-once — one make-up fire instead of a skip count */
    d = mk_due(
        "schema: jii.ambyte-schedule/v1-draft\n"
        "jobs:\n  j:\n    on: { cron: \"0 9 * * *\" }\n    missed: run-once\n"
        "    steps: [ { uses: device/log, with: { message: hi } } ]\n",
        time_sync_make(2026, 6, 21, 0, 0, 0), identity_localize, &p);
    if (d != NULL) {
        /* clock jumps to noon: the 09:00 slot is 3 h past grace */
        CHECK(sched_due_poll(d, time_sync_make(2026, 6, 21, 12, 0, 0)) == 1);
        CHECK(d->jobs[0].skipped == 0);
        /* and the make-up consumed the slot: nothing until tomorrow 09:00 */
        CHECK(sched_due_poll(d, time_sync_make(2026, 6, 21, 12, 0, 1)) == 0);
        free(p);
        free(d);
    }

    /* window entry: on_enter fires once when the gate opens */
    d = mk_due(
        "schema: jii.ambyte-schedule/v1-draft\n"
        "jobs:\n  j:\n    on: { every: 5m }\n"
        "    when: { window: { from: \"10:00\", to: \"11:00\" } }\n"
        "    steps: [ { uses: device/log, with: { message: hi } } ]\n",
        time_sync_make(2026, 6, 21, 9, 55, 0), identity_localize, &p);
    if (d != NULL) {
        CHECK(sched_due_poll(d, time_sync_make(2026, 6, 21, 9, 58, 0)) == 0);
        CHECK(sched_due_poll(d, time_sync_make(2026, 6, 21, 10, 0, 0)) == 1); /* entry fire */
        CHECK(sched_due_poll(d, time_sync_make(2026, 6, 21, 10, 0, 30)) == 0); /* not twice */
        CHECK(sched_due_poll(d, time_sync_make(2026, 6, 21, 10, 5, 0)) == 1); /* grid resumes */
        CHECK(d->jobs[0].runs == 2);
        free(p);
        free(d);
    }

    /* backward clock step: dues only move forward, no refire */
    d = mk_due(
        "schema: jii.ambyte-schedule/v1-draft\n"
        "jobs:\n  j:\n    on: { every: 1m }\n"
        "    steps: [ { uses: device/log, with: { message: hi } } ]\n",
        t0, identity_localize, &p);
    if (d != NULL) {
        CHECK(sched_due_poll(d, t0 + 60) == 1);
        CHECK(sched_due_poll(d, t0 + 30) == 0); /* clock stepped back */
        CHECK(sched_due_poll(d, t0 + 60) == 0); /* same wall second again: no refire */
        CHECK(d->jobs[0].runs == 1);
        free(p);
        free(d);
    }

    /* boot fires once, after the gate opens if gated */
    d = mk_due(
        "schema: jii.ambyte-schedule/v1-draft\n"
        "jobs:\n  j:\n    on: [ boot ]\n"
        "    when: { window: { from: \"10:00\", to: \"11:00\" } }\n"
        "    steps: [ { uses: device/log, with: { message: hi } } ]\n",
        time_sync_make(2026, 6, 21, 9, 0, 0), identity_localize, &p);
    if (d != NULL) {
        CHECK(sched_due_poll(d, time_sync_make(2026, 6, 21, 9, 0, 1)) == 0); /* gated */
        CHECK(sched_due_poll(d, time_sync_make(2026, 6, 21, 10, 0, 0)) == 1); /* gate opens */
        CHECK(sched_due_poll(d, time_sync_make(2026, 6, 21, 10, 0, 1)) == 0); /* once */
        free(p);
        free(d);
    }

    /* dispatch never fires by itself */
    d = mk_due(
        "schema: jii.ambyte-schedule/v1-draft\n"
        "jobs:\n  j:\n    on: dispatch\n"
        "    steps: [ { uses: device/log, with: { message: hi } } ]\n",
        t0, identity_localize, &p);
    if (d != NULL) {
        CHECK(sched_due_poll(d, t0 + 3600) == 0);
        CHECK(sched_due_next(d, t0) == -1);
        free(p);
        free(d);
    }

    /* sun trigger: NL midsummer, sunset+30m ≈ 22:32 CEST */
    time_sync_set_location(52.173, 5.819, 0);
    time_sync_set_utc_offset_seconds(7200);
    d = mk_due(
        "schema: jii.ambyte-schedule/v1-draft\n"
        "jobs:\n  j:\n    on: [ { sun: sunset, offset: +30m } ]\n"
        "    steps: [ { uses: device/log, with: { message: hi } } ]\n",
        time_sync_make(2026, 6, 21, 12, 0, 0), identity_localize, &p);
    if (d != NULL) {
        int64_t set, expect = 0;
        CHECK(time_sync_sun_on_date(time_sync_make(2026, 6, 21, 12, 0, 0),
                                    TIME_SYNC_SUNSET, &set) == ESP_OK);
        expect = set + 1800;
        CHECK(sched_due_next(d, time_sync_make(2026, 6, 21, 12, 0, 0)) == expect);
        CHECK(sched_due_poll(d, expect) == 1);
        free(p);
        free(d);
    }
}

/* DST through the due model with the libc offset: fall-back fires the
 * doubled wall minute once; spring-forward counts the nonexistent slot as
 * skipped, never fired. */
static void test_due_dst(void)
{
    setenv("TZ", "Europe/Amsterdam", 1);
    tzset();
    const char *yaml =
        "schema: jii.ambyte-schedule/v1-draft\n"
        "jobs:\n  j:\n    on: { cron: \"30 2 * * *\" }\n"
        "    steps: [ { uses: device/log, with: { message: hi } } ]\n";

    struct tm day = { 0 };
    day.tm_year = 126; day.tm_mon = 9; day.tm_mday = 24; /* 2026-10-24 UTC */
    time_t t0 = timegm(&day);
    sched_program_t *p;
    sched_due_t *d = mk_due(yaml, t0, libc_localize, &p);
    CHECK(d != NULL);
    if (d != NULL) {
        int fires = 0;
        for (time_t u = t0; u < t0 + 3 * 86400; u += 60) {
            fires += sched_due_poll(d, u) ? 1 : 0;
        }
        CHECK(fires == 3); /* 24th, 25th (once!), 26th */
        free(p);
        free(d);
    }

    day.tm_year = 126; day.tm_mon = 2; day.tm_mday = 28; /* 2026-03-28 UTC */
    t0 = timegm(&day);
    d = mk_due(yaml, t0, libc_localize, &p);
    CHECK(d != NULL);
    if (d != NULL) {
        int fires = 0;
        for (time_t u = t0; u < t0 + 3 * 86400; u += 60) {
            fires += sched_due_poll(d, u) ? 1 : 0;
        }
        CHECK(fires == 2);               /* 28th and 30th; 29th never on the wall */
        CHECK(d->jobs[0].skipped == 1);  /* the nonexistent slot is counted */
        free(p);
        free(d);
    }
    setenv("TZ", "UTC", 1);
    tzset();
}

int main(void)
{
    test_parser_acceptance();
    test_parser_rejections();
    test_compiler_rules();
    test_cron_basics();
    test_cron_dst();
    test_windows();
    test_due_model();
    test_due_dst();

    printf("SIZES sched_program_t=%zu job=%zu trigger=%zu entry=%zu protocol=%zu "
           "cron=%zu window=%zu due=%zu due_job=%zu\n",
           sizeof(sched_program_t), sizeof(sched_job_t), sizeof(sched_trigger_t),
           sizeof(sched_entry_t), sizeof(sched_protocol_t), sizeof(sched_cron_t),
           sizeof(sched_window_t), sizeof(sched_due_t), sizeof(sched_due_job_t));

    if (s_fails != 0) {
        printf("SCHED_SPEC_HOST_FAIL %d/%d checks failed\n", s_fails, s_checks);
        return 1;
    }
    printf("SCHED_SPEC_HOST_OK %d checks\n", s_checks);
    return 0;
}
