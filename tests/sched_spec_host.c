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
    /* mixed block/flow (review T2-4): the flow collection on a key's own
     * line shares the key's depth; further flow nesting counts +1 each */
    CHECK(parse_only_ok("a:\n  b:\n    c:\n      d:\n        e:\n          f: [0]\n"));
    PARSE_ERR("a:\n  b:\n    c:\n      d:\n        e:\n          f: [[0]]\n",
              "nesting deeper");
    /* the review's depth-11 document */
    PARSE_ERR("a:\n  b:\n    c:\n      d:\n        e:\n          f: [[[[[0]]]]]\n",
              "nesting deeper");

    /* int64 / duration magnitude is checked BEFORE any multiply (review
     * T2-1: "999999999999999999h" was signed-overflow UB) */
    PARSE_ERR("a: 999999999999999999h\n", "duration out of range");
    /* digits alone overflow int64 before the unit is even seen: the
     * documented "integer out of range" verdict for 19-digit shapes */
    PARSE_ERR("a: 9999999999999999999s\n", "integer out of range");
    PARSE_ERR("a: 9223372036854775807s\n", "duration out of range");
    CHECK(parse_only_ok("a: 9223372036854775s\n"));          /* exact max seconds */
    PARSE_ERR("a: 9223372036854776s\n", "duration out of range");
    CHECK(parse_only_ok("a: 153722867280912m\n"));           /* exact max minutes */
    PARSE_ERR("a: 153722867280913m\n", "duration out of range");
    PARSE_ERR("a: 2562047788016h\n", "duration out of range"); /* MAX/3.6e6 + 1 */
    CHECK(parse_only_ok("a: 2562047788015h\n"));             /* exact max hours */
    CHECK(parse_only_ok("a: 9223372036854775807ms\n"));
    PARSE_ERR("a: 9223372036854775808ms\n", "integer out of range");
    /* numeric-shaped but beyond int64: an error, never a silent string */
    PARSE_ERR("a: 9223372036854775808\n", "integer out of range");  /* INT64_MAX + 1 */
    PARSE_ERR("a: 9999999999999999999\n", "integer out of range");  /* 19 digits */
    PARSE_ERR("a: -9223372036854775809\n", "integer out of range");
    /* Accepted parser limitation: the negative magnitude is checked against
     * INT64_MAX, so INT64_MIN itself is deliberately not expressible. */
    PARSE_ERR("a: -9223372036854775808\n", "integer out of range");
    /* boundary: INT64_MAX itself parses */
    {
        sched_yaml_doc_t *dmax = NULL;
        char emax[128];
        const char *ymax = "a: 9223372036854775807\n";
        CHECK(sched_yaml_parse(ymax, strlen(ymax), &dmax, emax, sizeof(emax)) == ESP_OK);
        const sched_node_t *rmax = sched_yaml_root(dmax);
        CHECK(rmax != NULL && rmax->u.m.pairs[0].value->scal_kind == SCHED_SCAL_INT &&
              rmax->u.m.pairs[0].value->u.s.i == INT64_MAX);
        sched_yaml_free(dmax);
    }
    {
        sched_yaml_doc_t *dmin = NULL;
        char emin[128];
        const char *ymin = "a: -9223372036854775807\n";
        CHECK(sched_yaml_parse(ymin, strlen(ymin), &dmin, emin, sizeof(emin)) == ESP_OK);
        const sched_node_t *rmin = sched_yaml_root(dmin);
        CHECK(rmin != NULL && rmin->u.m.pairs[0].value->scal_kind == SCHED_SCAL_INT &&
              rmin->u.m.pairs[0].value->u.s.i == -INT64_MAX);
        sched_yaml_free(dmin);
    }

    /* quoted-scalar grammar (review): unterminated quotes are errors even in
     * block sequence items; the only escape is \" */
    PARSE_ERR("a: \"unterminated\n", "unterminated quoted string");
    PARSE_ERR("a: 'unterminated\n", "unterminated quoted string");
    PARSE_ERR("a:\n  - \"unterminated\n", "unterminated quoted string");
    PARSE_ERR("a: \"bad \\n escape\"\n", "unsupported escape");
    PARSE_ERR("a: \"bad \\\\ escape\"\n", "unsupported escape");
    PARSE_ERR("a: \"bad \\u1234 escape\"\n", "unsupported escape");

    /* string pool exact boundary: key "a" costs 2, each quoted item costs
     * len + 1 — land the total on 8192 exactly, then one byte over */
    {
        char *y = malloc(16 + 82 * 120);
        char item[120];
        strcpy(y, "a:\n");
        for (int i = 0; i < 81; i++) {           /* 81 × 101 = 8181 */
            snprintf(item, sizeof(item), "  - '%.100d'\n", i);
            strcat(y, item);
        }
        strcat(y, "  - '12345678'\n");           /* + 9 → 8192 exactly */
        CHECK(parse_only_ok(y));
        strcpy(y, "a:\n");
        for (int i = 0; i < 81; i++) {
            snprintf(item, sizeof(item), "  - '%.100d'\n", i);
            strcat(y, item);
        }
        strcat(y, "  - '123456789'\n");          /* + 10 → 8193: over */
        PARSE_ERR(y, "string pool limit");

        /* An empty quoted scalar still costs its terminating NUL. Exercise
         * that closing-quote path after an exact 8192-byte prefix. */
        y[0] = '\0';
        for (int i = 0; i < 81; i++) {
            snprintf(item, sizeof(item), "- '%.100d'\n", i);
            strcat(y, item);
        }
        strcat(y, "- '1234567890'\n");           /* 8181 + 11 = 8192 */
        strcat(y, "- ''\n");                     /* NUL makes 8193 */
        PARSE_ERR(y, "string pool limit");
        free(y);
    }
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
        "data: { runs: $job.runs, saturated: $job.skipped_saturated, note: hello, n: 5, x: 1.5, ok: true } } } ]\n");
    CHECK(p != NULL);
    if (p != NULL) {
        const sched_step_t *st = &p->jobs[0].steps[0];
        CHECK(st->entry_count == 7); /* kind + 6 data entries */
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
    /* Signed sun/window modifiers use a separate compiler parser from YAML's
     * duration lexer. It must require at least one digit and guard magnitude
     * before every decimal/unit multiplication as well. */
    COMPILE_ERR("schema: jii.ambyte-schedule/v1-draft\njobs:\n  j:\n"
                "    on: { sun: sunrise, offset: \"+h\" }\n"
                "    steps: [ { uses: device/log, with: { message: hi } } ]\n",
                "expected a duration");
    COMPILE_ERR("schema: jii.ambyte-schedule/v1-draft\njobs:\n  j:\n"
                "    on: { sun: sunrise, offset: \"+9223372036854775808ms\" }\n"
                "    steps: [ { uses: device/log, with: { message: hi } } ]\n",
                "duration too large");
    COMPILE_ERR("schema: jii.ambyte-schedule/v1-draft\njobs:\n  j:\n"
                "    on: { sun: sunrise, offset: \"+999999999999999999999999999999999999h\" }\n"
                "    steps: [ { uses: device/log, with: { message: hi } } ]\n",
                "duration too large");
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

    /* review T2-3: protocol references resolve for EVERY trigger kind,
     * not only inside the every-trigger duration check */
    COMPILE_ERR("schema: jii.ambyte-schedule/v1-draft\njobs:\n  j:\n"
                "    on: [ boot ]\n"
                "    steps: [ { uses: ambit/trace, with: { protocol: NOPE } } ]\n",
                "unknown protocol");
    COMPILE_ERR("schema: jii.ambyte-schedule/v1-draft\njobs:\n  j:\n"
                "    on: { cron: \"0 9 * * *\" }\n"
                "    steps: [ { uses: ambit/trace, with: { protocol: NOPE } } ]\n",
                "unknown protocol");
    COMPILE_ERR("schema: jii.ambyte-schedule/v1-draft\njobs:\n  j:\n"
                "    on: { sun: sunset }\n"
                "    steps: [ { uses: ambit/trace, with: { protocol: NOPE } } ]\n",
                "unknown protocol");
    COMPILE_ERR("schema: jii.ambyte-schedule/v1-draft\njobs:\n  j:\n"
                "    on: { at: \"08:00\" }\n"
                "    steps: [ { uses: ambit/trace, with: { protocol: NOPE } } ]\n",
                "unknown protocol");
    COMPILE_ERR("schema: jii.ambyte-schedule/v1-draft\njobs:\n  j:\n"
                "    on: { weekly: { days: [mon], at: \"08:00\" } }\n"
                "    steps: [ { uses: ambit/trace, with: { protocol: NOPE } } ]\n",
                "unknown protocol");
    COMPILE_ERR("schema: jii.ambyte-schedule/v1-draft\njobs:\n  j:\n"
                "    on: dispatch\n"
                "    steps: [ { uses: ambit/trace, with: { protocol: NOPE } } ]\n",
                "unknown protocol");

    /* header provenance keys are string scalars only (review): 123 / true /
     * 30m keep .str set for every scalar kind, so node_text proves nothing */
    COMPILE_ERR("schema: jii.ambyte-schedule/v1-draft\nid: 123\njobs:\n  j:\n"
                "    on: { every: 5m }\n"
                "    steps: [ { uses: device/log, with: { message: hi } } ]\n",
                "'id' must be a string");
    COMPILE_ERR("schema: jii.ambyte-schedule/v1-draft\nversion: true\njobs:\n  j:\n"
                "    on: { every: 5m }\n"
                "    steps: [ { uses: device/log, with: { message: hi } } ]\n",
                "'version' must be a string");
    COMPILE_ERR("schema: jii.ambyte-schedule/v1-draft\nworkbookVersionId: 30m\njobs:\n  j:\n"
                "    on: { every: 5m }\n"
                "    steps: [ { uses: device/log, with: { message: hi } } ]\n",
                "'workbookVersionId' must be a string");
    /* and string provenance is carried through */
    p = COMPILE_OK(
        "schema: jii.ambyte-schedule/v1-draft\n"
        "id: urn:jii:schedule:default-multichannel\n"
        "version: 0.1.0-draft\n"
        "workbookVersionId: 7b281a2e-d86a-4cc6-8268-81b67315f1ad\n"
        "jobs:\n  j:\n    on: { every: 5m }\n"
        "    steps: [ { uses: device/log, with: { message: hi } } ]\n");
    CHECK(p != NULL);
    if (p != NULL) {
        const char *id = sched_pool_str(p, p->id_off);
        const char *ver = sched_pool_str(p, p->version_off);
        const char *wbv = sched_pool_str(p, p->workbook_version_id_off);
        CHECK(id != NULL && strcmp(id, "urn:jii:schedule:default-multichannel") == 0);
        CHECK(ver != NULL && strcmp(ver, "0.1.0-draft") == 0);
        CHECK(wbv != NULL && strcmp(wbv, "7b281a2e-d86a-4cc6-8268-81b67315f1ad") == 0);
        free(p);
    }

    /* strict quoted HH:MM (review): partial matches and extra fields reject */
    COMPILE_ERR("schema: jii.ambyte-schedule/v1-draft\njobs:\n  j:\n"
                "    on: { at: \"1:2x\" }\n"
                "    steps: [ { uses: device/log, with: { message: hi } } ]\n",
                "expected HH:MM");
    COMPILE_ERR("schema: jii.ambyte-schedule/v1-draft\njobs:\n  j:\n"
                "    on: { at: \"1:2\" }\n"
                "    steps: [ { uses: device/log, with: { message: hi } } ]\n",
                "expected HH:MM");
    COMPILE_ERR("schema: jii.ambyte-schedule/v1-draft\njobs:\n  j:\n"
                "    on: { at: \"10:00:00\" }\n"
                "    steps: [ { uses: device/log, with: { message: hi } } ]\n",
                "expected HH:MM");
    COMPILE_ERR("schema: jii.ambyte-schedule/v1-draft\njobs:\n  j:\n"
                "    on: [ boot ]\n"
                "    when: { window: { from: \"9:5x\", to: sunset } }\n"
                "    steps: [ { uses: device/log, with: { message: hi } } ]\n",
                "window edge");
    /* sane quoted clocks still compile */
    p = COMPILE_OK("schema: jii.ambyte-schedule/v1-draft\njobs:\n  j:\n"
                   "    on: [ { at: \"08:00\" }, { at: \"8:05\" } ]\n"
                   "    steps: [ { uses: device/log, with: { message: hi } } ]\n");
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
    /* A numeric cron token is bounded input too; reject it without overflowing
     * the int accumulator used by read_number(). */
    CHECK(sched_cron_parse("999999999999999999999999999999999999 * * * *",
                           &c, err, sizeof(err)) == ESP_FAIL);

    /* review: "*****" without separators is not cron … */
    CHECK(sched_cron_parse("*****", &c, err, sizeof(err)) == ESP_FAIL);
    /* … while case-insensitive names that CONTAIN L or W are names */
    CHECK(sched_cron_parse("0 9 * JUL WED", &c, err, sizeof(err)) == ESP_OK);
    CHECK(c.month == (1U << 7));  /* July */
    CHECK(c.dow == (1U << 3));    /* Wednesday */
    CHECK(c.dom_restricted == 0 && c.dow_restricted == 1);
    /* but L/W as field syntax stays rejected, at any position */
    CHECK(sched_cron_parse("0 0 15W * *", &c, err, sizeof(err)) == ESP_FAIL);
    CHECK(sched_cron_parse("0 0 L-2 * *", &c, err, sizeof(err)) == ESP_FAIL);
    CHECK(sched_cron_parse("0 0 * * 5L", &c, err, sizeof(err)) == ESP_FAIL);

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

    /* More than the 4096-slot inner walk cap: the outer loop must continue
     * without dropping debt. Thirty-five days on a 10-minute grid has 5040
     * elapsed slots; 5039 are stale and the current slot fires. */
    d = mk_due(
        "schema: jii.ambyte-schedule/v1-draft\n"
        "jobs:\n  j:\n    on: { every: 10m }\n"
        "    steps: [ { uses: device/log, with: { message: hi } } ]\n",
        t0, identity_localize, &p);
    if (d != NULL) {
        CHECK(sched_due_poll(d, t0 + 35 * 86400) == 1);
        CHECK(d->jobs[0].skipped == 5039);
        free(p);
        free(d);
    }

    /* The due model has one-second resolution. Reject the 1500 ms grid that
     * used to be quantised +2,+3,+5,+6,+8,+9,+11 and then miscounted by the
     * arithmetic stale-slot shortcut. */
    COMPILE_ERR(
        "schema: jii.ambyte-schedule/v1-draft\n"
        "jobs:\n  j:\n    on: { every: 1500ms }\n"
        "    steps: [ { uses: device/log, with: { message: hi } } ]\n",
        "whole number of seconds");
    COMPILE_ERR(
        "schema: jii.ambyte-schedule/v1-draft\n"
        "jobs:\n  j:\n    on: { every: 2500ms }\n"
        "    steps: [ { uses: device/log, with: { message: hi } } ]\n",
        "whole number of seconds");
    COMPILE_ERR(
        "schema: jii.ambyte-schedule/v1-draft\n"
        "jobs:\n  j:\n    on: { every: 90s, phase: 1500ms }\n"
        "    steps: [ { uses: device/log, with: { message: hi } } ]\n",
        "whole number of seconds");

    /* review T2-2, the exact case: 10 m grid behind a daily 10:00–11:00
     * window, init 10:00, poll ~26 h later. The gate flips twice inside the
     * stale interval; only the runnable slots count (5 left on day 1 + 6 on
     * day 2 = 11), never the whole 155-slot interval arithmetically. */
    d = mk_due(
        "schema: jii.ambyte-schedule/v1-draft\n"
        "jobs:\n  j:\n    on: { every: 10m }\n"
        "    when: { window: { from: \"10:00\", to: \"11:00\" } }\n"
        "    steps: [ { uses: device/log, with: { message: hi } } ]\n",
        time_sync_make(2026, 6, 21, 10, 0, 0), identity_localize, &p);
    if (d != NULL) {
        CHECK(sched_due_poll(d, time_sync_make(2026, 6, 22, 12, 0, 0)) == 0);
        CHECK(d->jobs[0].skipped == 11);
        free(p);
        free(d);
    }

    /* same staleness with missed: run-once — the make-up fire happens iff a
     * missed slot had its gate open at ITS due time (not the first stale
     * one's), then the debt is consumed */
    d = mk_due(
        "schema: jii.ambyte-schedule/v1-draft\n"
        "jobs:\n  j:\n    on: { every: 10m }\n    missed: run-once\n"
        "    when: { window: { from: \"10:00\", to: \"11:00\" } }\n"
        "    steps: [ { uses: device/log, with: { message: hi } } ]\n",
        time_sync_make(2026, 6, 21, 10, 0, 0), identity_localize, &p);
    if (d != NULL) {
        CHECK(sched_due_poll(d, time_sync_make(2026, 6, 22, 12, 0, 0)) == 1);
        CHECK(d->jobs[0].skipped == 0);
        CHECK(sched_due_poll(d, time_sync_make(2026, 6, 22, 12, 0, 1)) == 0);
        free(p);
        free(d);
    }

    /* first stale slot OUTSIDE the gate: the walk must still find the
     * runnable slots later in the interval (day 2's 6), not coalesce from
     * the first slot's gate state */
    d = mk_due(
        "schema: jii.ambyte-schedule/v1-draft\n"
        "jobs:\n  j:\n    on: { every: 10m }\n"
        "    when: { window: { from: \"10:00\", to: \"11:00\" } }\n"
        "    steps: [ { uses: device/log, with: { message: hi } } ]\n",
        time_sync_make(2026, 6, 21, 12, 0, 0), identity_localize, &p);
    if (d != NULL) {
        CHECK(sched_due_poll(d, time_sync_make(2026, 6, 22, 12, 0, 0)) == 0);
        CHECK(d->jobs[0].skipped == 6);
        free(p);
        free(d);
    }

    /* unresolved sun window (polar day at 78° N): unresolved: skip means no
     * slot is runnable — nothing counted, nothing fired, dues still advance */
    time_sync_set_location(78.0, 15.6, 0);
    time_sync_set_utc_offset_seconds(7200);
    d = mk_due(
        "schema: jii.ambyte-schedule/v1-draft\n"
        "jobs:\n  j:\n    on: { every: 10m }\n"
        "    when: { window: { from: sunrise, to: sunset } }\n"
        "    steps: [ { uses: device/log, with: { message: hi } } ]\n",
        time_sync_make(2026, 6, 21, 12, 0, 0), identity_localize, &p);
    if (d != NULL) {
        CHECK(sched_due_poll(d, time_sync_make(2026, 6, 22, 12, 0, 0)) == 0);
        CHECK(d->jobs[0].skipped == 0);
        free(p);
        free(d);
    }

    time_sync_set_location(52.173, 5.819, 0);
    time_sync_set_utc_offset_seconds(7200);

    /* The shared work budget must not make run-once semantics depend on job
     * order.  Job a consumes the 3600 evaluations; job b still had runnable
     * stale slots in the same daily window and therefore owes one make-up. */
    d = mk_due(
        "schema: jii.ambyte-schedule/v1-draft\n"
        "jobs:\n"
        "  a:\n    on: { every: 1s }\n"
        "    when: { window: { from: \"10:00\", to: \"11:00\" } }\n"
        "    steps: [ { uses: device/log, with: { message: a } } ]\n"
        "  b:\n    on: { every: 1s }\n    missed: run-once\n"
        "    when: { window: { from: \"10:00\", to: \"11:00\" } }\n"
        "    steps: [ { uses: device/log, with: { message: b } } ]\n",
        time_sync_make(2026, 6, 21, 10, 0, 0), identity_localize, &p);
    if (d != NULL) {
        CHECK(sched_due_poll(d, time_sync_make(2026, 6, 22, 12, 0, 0)) == 2);
        CHECK(d->jobs[0].skipped_saturated == 1);
        CHECK(d->jobs[1].skipped_saturated == 1);
        free(p);
        free(d);
    }

    /* The shipped legacy shape has a saturated 1 Hz daytime job ahead of an
     * hourly cron job. Each job owns its 3600-step budget, so a 2 h poll
     * gap counts both stale hourly slots without inheriting saturation. */
    d = mk_due(
        "schema: jii.ambyte-schedule/v1-draft\n"
        "jobs:\n"
        "  fast:\n    on: { every: 1s }\n"
        "    when: { window: { from: sunrise-1h, to: sunset+1h, unresolved: skip } }\n"
        "    steps: [ { uses: ambit/spectrum, with: { channels: [0] } } ]\n"
        "  slow:\n    on: { every: 10m }\n"
        "    when: { window: { from: sunset+1h, to: sunrise-1h, unresolved: run } }\n"
        "    steps: [ { uses: ambit/spectrum, with: { channels: [0] } } ]\n"
        "  hourly_status:\n    on: { cron: \"0 * * * *\" }\n"
        "    steps: [ { uses: device/status-report } ]\n",
        time_sync_make(2026, 6, 21, 11, 15, 0), identity_localize, &p);
    if (d != NULL) {
        CHECK(sched_due_poll(d, time_sync_make(2026, 6, 21, 13, 15, 0)) == 0x1);
        CHECK(d->jobs[0].skipped_saturated == 1);
        CHECK(d->jobs[2].skipped == 2);
        CHECK(d->jobs[2].skipped_saturated == 0);
        free(p);
        free(d);
    }

    /* skipped is saturating telemetry even when an absurd RTC jump crosses
     * UINT32_MAX occurrences. */
    d = mk_due(
        "schema: jii.ambyte-schedule/v1-draft\n"
        "jobs:\n  j:\n    on: { every: 1s }\n"
        "    steps: [ { uses: device/log, with: { message: hi } } ]\n",
        t0, identity_localize, &p);
    if (d != NULL) {
        CHECK(sched_due_poll(d, t0 + (int64_t)UINT32_MAX + 10000000) == 1);
        CHECK(d->jobs[0].skipped == UINT32_MAX);
        CHECK(d->jobs[0].skipped_saturated == 1);
        free(p);
        free(d);
    }

    /* A polar sunrise initially resolves to -1. Polling after the event
     * returns must re-resolve it, and the job then fires at that due. */
    time_sync_set_location(78.2, 15.6, 0);
    time_sync_set_utc_offset_seconds(7200);
    d = mk_due(
        "schema: jii.ambyte-schedule/v1-draft\n"
        "jobs:\n  j:\n    on: { sun: sunrise }\n"
        "    steps: [ { uses: device/log, with: { message: hi } } ]\n",
        time_sync_make(2026, 6, 21, 12, 0, 0), identity_localize, &p);
    if (d != NULL) {
        CHECK(d->jobs[0].wall_due_local[0] == -1);
        int64_t august = time_sync_make(2026, 8, 25, 0, 0, 0);
        CHECK(sched_due_poll(d, august) == 0);
        CHECK(d->jobs[0].wall_due_local[0] > august);
        CHECK(sched_due_poll(d, d->jobs[0].wall_due_local[0]) == 1);
        free(p);
        free(d);
    }

    time_sync_set_location(52.173, 5.819, 0);
    time_sync_set_utc_offset_seconds(7200);

    /* re-review 2-2 pin: the shipped 1 Hz sun-gated shape after a
     * three-week gap is ~1.8 M stale slots. The per-poll walk budget bounds
     * the work: the backlog is jumped to the grace horizon, skipped stays a
     * lower bound, and skipped_saturated reports it. */
    d = mk_due(
        "schema: jii.ambyte-schedule/v1-draft\n"
        "jobs:\n  j:\n    on: { every: 1s }\n"
        "    when: { window: { from: sunrise, to: sunset } }\n"
        "    steps: [ { uses: device/log, with: { message: hi } } ]\n",
        time_sync_make(2026, 6, 1, 12, 0, 0), identity_localize, &p);
    if (d != NULL) {
        struct timespec ta, tb;
        clock_gettime(CLOCK_MONOTONIC, &ta);
        sched_due_poll(d, time_sync_make(2026, 6, 22, 12, 0, 0));
        clock_gettime(CLOCK_MONOTONIC, &tb);
        double ms = (double)(tb.tv_sec - ta.tv_sec) * 1e3 +
                    (double)(tb.tv_nsec - ta.tv_nsec) / 1e6;
        printf("MISSED_WALK_3W_MS %.3f\n", ms);
        CHECK(ms < 10.0); /* unbudgeted this was ~650 ms on this host */
        CHECK(d->jobs[0].skipped_saturated == 1);
        CHECK(d->jobs[0].skipped > 0 && d->jobs[0].skipped <= 3600);
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

/* ── clock-step re-anchor (T3 review blocker 1) ────────────────────────── */

static void test_due_reanchor(void)
{
    sched_program_t *p;
    int64_t t0 = time_sync_make(2026, 6, 21, 0, 0, 0); /* grid-aligned */
    const char *every_1m =
        "schema: jii.ambyte-schedule/v1-draft\n"
        "jobs:\n  j:\n    on: { every: 1m }\n"
        "    steps: [ { uses: device/log, with: { message: hi } } ]\n";

    /* backward 1 h correction: without re-anchor the stale due (t0+120)
     * would silence the 1 m job for ~1 h; re-anchor resumes it within one
     * period on the new timebase */
    sched_due_t *d = mk_due(every_1m, t0, identity_localize, &p);
    CHECK(d != NULL);
    if (d != NULL) {
        CHECK(sched_due_poll(d, t0 + 60) == 1);            /* fired; next due t0+120 */
        int64_t now = t0 + 60 - 3600;                      /* RTC corrected back 1 h */
        CHECK(sched_due_poll(d, now) == 0);                /* stale due: silent */
        sched_due_reanchor(d, now);
        CHECK(d->jobs[0].wall_due_local[0] == now + 60);   /* re-anchored grid */
        CHECK(sched_due_poll(d, now + 30) == 0);
        CHECK(sched_due_poll(d, now + 60) == 1);           /* resumes within a period */
        CHECK(d->jobs[0].skipped == 0);                    /* no fake missed slots */
        CHECK(d->jobs[0].runs == 2);
        free(p);
        free(d);
    }

    /* forward ~2 h correction: dues at/past the new now are LEFT for poll's
     * late-grace/missed accounting — counted skipped slots, then the in-grace
     * slot fires (grace for every 1m is the period, 60 s) */
    d = mk_due(every_1m, t0, identity_localize, &p);
    CHECK(d != NULL);
    if (d != NULL) {
        CHECK(sched_due_poll(d, t0 + 60) == 1);
        int64_t now = t0 + 7260;                           /* RTC corrected forward */
        sched_due_reanchor(d, now);
        CHECK(d->jobs[0].wall_due_local[0] == t0 + 120);   /* past due untouched */
        CHECK(sched_due_poll(d, now) == 1);                /* t0+7260 slot fires */
        CHECK(d->jobs[0].skipped == 119);                  /* t0+120..t0+7200 counted */
        free(p);
        free(d);
    }

    /* boot pending is not re-armed by a re-anchor (backward step after the
     * boot firing was consumed) */
    d = mk_due(
        "schema: jii.ambyte-schedule/v1-draft\n"
        "jobs:\n  j:\n    on: [ boot, { every: 1m } ]\n"
        "    steps: [ { uses: device/log, with: { message: hi } } ]\n",
        t0, identity_localize, &p);
    CHECK(d != NULL);
    if (d != NULL) {
        CHECK(sched_due_poll(d, t0) == 1);                 /* boot fires */
        CHECK(d->jobs[0].boot_pending == 0);
        sched_due_reanchor(d, t0 + 60 - 3600);
        CHECK(d->jobs[0].boot_pending == 0);               /* stays consumed */
        CHECK(sched_due_poll(d, t0 + 60 - 3600) == 0);     /* no boot refire */
        free(p);
        free(d);
    }

    /* fired-minute latch preserved: a sub-minute backward step re-anchors the
     * cron due into the minute that already fired, and the latch is what
     * prevents the double-fire */
    d = mk_due(
        "schema: jii.ambyte-schedule/v1-draft\n"
        "jobs:\n  j:\n    on: { cron: \"* * * * *\" }\n"
        "    steps: [ { uses: device/log, with: { message: hi } } ]\n",
        t0, identity_localize, &p);
    CHECK(d != NULL);
    if (d != NULL) {
        CHECK(sched_due_poll(d, t0 + 60) == 1);
        int64_t latch = d->jobs[0].fired_minute;
        sched_due_reanchor(d, t0 + 30);                    /* 30 s backward */
        CHECK(d->jobs[0].fired_minute == latch);           /* latch preserved */
        CHECK(d->jobs[0].wall_due_local[0] == t0 + 60);    /* re-anchored into */
        CHECK(sched_due_poll(d, t0 + 60) == 0);            /* ...but no refire */
        CHECK(sched_due_poll(d, t0 + 120) == 1);           /* next minute normal */
        CHECK(d->jobs[0].runs == 2);
        free(p);
        free(d);
    }

    /* gate state preserved and re-anchor without a step is a no-op on the
     * grid: windowed job mid-window keeps gate_open, no spurious entry edge */
    d = mk_due(
        "schema: jii.ambyte-schedule/v1-draft\n"
        "jobs:\n  j:\n    on: { every: 5m }\n"
        "    when: { window: { from: \"10:00\", to: \"11:00\" } }\n"
        "    steps: [ { uses: device/log, with: { message: hi } } ]\n",
        time_sync_make(2026, 6, 21, 9, 59, 0), identity_localize, &p);
    CHECK(d != NULL);
    if (d != NULL) {
        int64_t ten = time_sync_make(2026, 6, 21, 10, 0, 0);
        CHECK(sched_due_poll(d, ten) == 1);                /* on_enter */
        CHECK(d->jobs[0].gate_open == 1);
        int64_t due_before = d->jobs[0].wall_due_local[0];
        sched_due_reanchor(d, ten + 120);                  /* no clock step */
        CHECK(d->jobs[0].wall_due_local[0] == due_before); /* grid unchanged */
        CHECK(d->jobs[0].gate_open == 1);                  /* gate preserved */
        CHECK(sched_due_poll(d, ten + 300) == 1);          /* grid, no 2nd entry */
        CHECK(d->jobs[0].runs == 2);
        free(p);
        free(d);
    }
}

/* The runner's injected dual-clock seam: wall establishes/re-establishes the
 * anchor, while armed dues and cadence are monotonic microsecond instants. */
static void test_due_monotonic_clock(void)
{
    const char *every_1m =
        "schema: jii.ambyte-schedule/v1-draft\n"
        "jobs:\n  j:\n    on: { every: 1m }\n"
        "    steps: [ { uses: device/log, with: { message: hi } } ]\n";
    const int64_t us = 1000000;
    const int64_t mono0 = 5000000;
    const int64_t t0 = time_sync_make(2026, 6, 21, 0, 0, 0);
    sched_program_t *p = NULL;
    sched_due_t *d = calloc(1, sizeof(*d));
    char err[256];
    CHECK(d != NULL);
    p = calloc(1, sizeof(*p));
    CHECK(p != NULL);
    if (d == NULL || p == NULL) { free(d); free(p); return; }
    CHECK(sched_compile_text(every_1m, strlen(every_1m), p, err, sizeof(err)) == ESP_OK);

    sched_due_init_mono(d, p, identity_localize, NULL, t0, mono0);
    CHECK(d->jobs[0].due_us[0] == mono0 + 60 * us);

    /* A -2 s wall correction is inside the re-anchor threshold. At +60 s
     * monotonic the slot still fires exactly on cadence even though wall has
     * advanced only 58 s; the next armed due is another monotonic minute. */
    const int64_t mono1 = mono0 + 60 * us;
    CHECK(sched_due_project_wall_utc(d, mono1) == t0 + 60);
    CHECK((t0 + 58) - sched_due_project_wall_utc(d, mono1) == -2);
    CHECK(sched_due_poll_mono(d, mono1) == 1);
    CHECK(sched_due_next_mono(d, mono1) == mono0 + 120 * us);

    /* A real forward step (>2 s) reprojects the old armed slot onto the new
     * anchor so T2's exact missed-slot accounting remains intact. */
    const int64_t mono2 = mono1 + us;
    sched_due_reanchor_mono(d, t0 + 7260, mono2);
    CHECK(sched_due_poll_mono(d, mono2) == 1);
    CHECK(d->jobs[0].skipped == 119);
    free(d);
    free(p);

    /* Backward step: the obsolete future wall grid is re-resolved from the
     * corrected wall anchor and becomes an armed monotonic due within 60 s. */
    d = calloc(1, sizeof(*d));
    p = calloc(1, sizeof(*p));
    CHECK(d != NULL && p != NULL);
    if (d == NULL || p == NULL) { free(d); free(p); return; }
    CHECK(sched_compile_text(every_1m, strlen(every_1m), p, err, sizeof(err)) == ESP_OK);
    sched_due_init_mono(d, p, identity_localize, NULL, t0, mono0);
    CHECK(sched_due_poll_mono(d, mono1) == 1);
    sched_due_reanchor_mono(d, t0 + 60 - 3600, mono2);
    CHECK(sched_due_next_mono(d, mono2) == mono2 + 60 * us);
    CHECK(sched_due_poll_mono(d, mono2 + 59 * us) == 0);
    CHECK(sched_due_poll_mono(d, mono2 + 60 * us) == 1);
    CHECK(d->jobs[0].skipped == 0);
    free(d);
    free(p);
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

/* ── action table JSON dump ──────────────────────────────────────────── */

static int count_occurrences(const char *hay, const char *needle)
{
    int n = 0;
    size_t nl = strlen(needle);
    for (const char *s = hay; (s = strstr(s, needle)) != NULL; s += nl) n++;
    return n;
}

static void test_actions_dump(void)
{
    /* NULL/0 size probe: nothing written, return is the needed length */
    size_t need = sched_actions_dump_json(NULL, 0);
    CHECK(need > 100);
    char *buf = malloc(need + 1);
    memset(buf, 0xAA, need + 1);
    size_t got = sched_actions_dump_json(buf, need + 1);
    CHECK(got == need);
    CHECK(buf[need] == '\0');
    /* actions with required inputs require the outer `with` (review):
     * trace/actinic/store-event/log/sleep do; spectrum/leaf-temp/
     * status-report must not */
    CHECK(count_occurrences(buf, "\"required\":[\"uses\",\"with\"]") == 5);
    CHECK(count_occurrences(buf, "\"required\":[\"uses\"]") == 3);
    /* truncation reports the would-be length (snprintf semantics); scratch
     * buffer so the counts above stay valid */
    {
        char scratch[16];
        CHECK(sched_actions_dump_json(scratch, sizeof(scratch)) == need);
    }
    free(buf);
}

int main(void)
{
    test_parser_acceptance();
    test_parser_rejections();
    test_cron_basics();
    test_compiler_rules();
    test_cron_dst();
    test_windows();
    test_due_model();
    test_due_reanchor();
    test_due_monotonic_clock();
    test_due_dst();
    test_actions_dump();

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
