/*
 * envelope_provenance_host.c — unit runner for the publish envelope's
 * workbook-provenance part and the three production envelope format strings,
 * compiled by tests/test_envelope_provenance.py (pattern: tests/
 * payload_v3_host.c). Pure host C11; the code under test is the exact
 * production envelope_provenance.c (plus the real payload_scalar.c it routes
 * with) + the real DC_*_ENVELOPE_FMT macros, so a malformed C format key
 * cannot slip past the Python-side reconstructions.
 *
 * Prints "ENVELOPE_PROVENANCE_HOST_OK <checks>" and exits 0 when all checks
 * pass; before that it prints rendered envelopes as KEY=<json> lines for the
 * Python side to json-parse.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "envelope_provenance.h"

static int s_checks, s_fails;
#define CHECK(cond)                                                     \
    do {                                                                \
        s_checks++;                                                     \
        if (!(cond)) {                                                  \
            s_fails++;                                                  \
            printf("FAIL %d: %s\n", __LINE__, #cond);                   \
        }                                                               \
    } while (0)

#define WB_ID "1c7b82b5-a0d4-4b1d-bcd6-6e0b7d470040"

/* Row fixtures. Only the keys payload_scalar addresses matter here; the real
 * builder bytes are exercised in tests/payload_scalar_host.c. */
#define TRACE_ROW                                                       \
    "{\"schema\":\"ambit.trace/3\",\"measure_id\":26337,"               \
    "\"channel\":\"uart_0\",\"device\":\"AmbitV003\","                  \
    "\"sensor_id\":\"10:91:A8:4F:4F:C0\",\"tag\":\"MEASUREMENT\","      \
    "\"protocol\":{\"name\":\"MPF\",\"tag\":\"edge\"}}"
#define SPECTRUM_ROW                                                    \
    "{\"schema\":\"ambit.spectrum/1\",\"measure_id\":65928,"            \
    "\"channel\":\"uart_0\",\"device\":\"AD81\",\"tag\":\"MEASUREMENT\"}"
#define TELEMETRY_ROW                                                   \
    "{\"schema\":\"ambyte.telemetry/1\",\"measure_id\":26338,"          \
    "\"device\":\"28:37:2F:FF:E7:04\",\"tag\":\"TELEMETRY\"}"
#define V2_ROW                                                          \
    "{\"v\":2,\"tag\":\"MEASUREMENT\",\"channel\":\"uart_1\","          \
    "\"device\":\"AmbitV003\",\"data\":{\"s_630\":[1,2,3]}}"

static schedule_provenance_t prov_with_macros(int n)
{
    schedule_provenance_t p;
    memset(&p, 0, sizeof(p));
    snprintf(p.workbook_version_id, sizeof(p.workbook_version_id), "%s", WB_ID);
    p.macro_count = (uint8_t)n;
    for (int i = 0; i < n; i++) {
        snprintf(p.macros[i].id, sizeof(p.macros[i].id),
                 "47b03f78-a0d4-4b1d-bcd6-6e0b7d47004%d", i);
        snprintf(p.macros[i].name, sizeof(p.macros[i].name), "ambyte-trace-%d", i);
        snprintf(p.macros[i].filename, sizeof(p.macros[i].filename), "macro_%08x", i);
    }
    return p;
}

/* Append one `when:` condition to a macro, mirroring what sched_compile.c
 * materializes through the domain port. */
static void add_cond(schedule_provenance_t *p, int macro, uint8_t field,
                     uint8_t op, const char *value)
{
    int j = p->macros[macro].cond_count++;
    p->macros[macro].conds[j].field = field;
    p->macros[macro].conds[j].op = op;
    snprintf(p->macros[macro].conds[j].value,
             sizeof(p->macros[macro].conds[j].value), "%s", value);
}

/* Rename a macro so the per-row assertions read like the workbook does. */
static void name_macro(schedule_provenance_t *p, int i, const char *name,
                       const char *filename)
{
    snprintf(p->macros[i].name, sizeof(p->macros[i].name), "%s", name);
    snprintf(p->macros[i].filename, sizeof(p->macros[i].filename), "%s", filename);
}

/* The header the design targets: one macro per payload family, each routed by
 * a single `schema eq` condition compiled from a workbook branch path. */
static schedule_provenance_t prov_routed_by_schema(void)
{
    schedule_provenance_t p = prov_with_macros(3);
    name_macro(&p, 0, "ambyte-trace", "macro_trace01");
    add_cond(&p, 0, SCHED_PROV_COND_FIELD_SCHEMA, SCHED_PROV_COND_OP_EQ,
             "ambit.trace/3");
    name_macro(&p, 1, "ambyte-spectrum", "macro_spec02");
    add_cond(&p, 1, SCHED_PROV_COND_FIELD_SCHEMA, SCHED_PROV_COND_OP_EQ,
             "ambit.spectrum/1");
    name_macro(&p, 2, "ambyte-telemetry", "macro_tele03");
    add_cond(&p, 2, SCHED_PROV_COND_FIELD_SCHEMA, SCHED_PROV_COND_OP_EQ,
             "ambyte.telemetry/1");
    return p;
}

/* The macros key of a rendered part, or NULL: lets a check assert the exact
 * per-row macro list without re-deriving the whole splice. */
static int macro_entries(const char *part)
{
    const char *p = strstr(part, "\"macros\":[");
    if (p == NULL) return 0;
    int n = 0;
    for (; *p != '\0' && *p != ']'; p++) {
        if (*p == '{') n++;
    }
    return n;
}

int main(void)
{
    char part[1440];

    /* unset everything → empty part (envelope byte-identical to today) */
    schedule_provenance_t empty;
    memset(&empty, 0, sizeof(empty));
    CHECK(envelope_provenance_part(&empty, TRACE_ROW, part, sizeof(part)) == 0);
    CHECK(part[0] == '\0');
    CHECK(envelope_provenance_part(NULL, TRACE_ROW, part, sizeof(part)) == 0);

    /* workbook only */
    schedule_provenance_t wb_only;
    memset(&wb_only, 0, sizeof(wb_only));
    snprintf(wb_only.workbook_version_id, sizeof(wb_only.workbook_version_id),
             "%s", WB_ID);
    int n = envelope_provenance_part(&wb_only, TRACE_ROW, part, sizeof(part));
    CHECK(n == (int)strlen(part));
    CHECK(strcmp(part, "\"workbook_version_id\":\"" WB_ID "\",") == 0);

    /* workbook + one unconditioned macro: exact bytes, and a NULL payload is
     * still fine — no condition means nothing is ever scanned (every
     * already-stamped schedule keeps this exact rendering) */
    schedule_provenance_t one = prov_with_macros(1);
    n = envelope_provenance_part(&one, TRACE_ROW, part, sizeof(part));
    CHECK(n == (int)strlen(part));
    CHECK(strcmp(part,
                 "\"workbook_version_id\":\"" WB_ID "\","
                 "\"macros\":[{\"id\":\"47b03f78-a0d4-4b1d-bcd6-6e0b7d470040\","
                 "\"name\":\"ambyte-trace-0\",\"filename\":\"macro_00000000\"}],") == 0);
    char nullpart[1440];
    CHECK(envelope_provenance_part(&one, NULL, nullpart, sizeof(nullpart)) == n);
    CHECK(strcmp(nullpart, part) == 0);

    /* eight macros: bounded, valid, count preserved */
    schedule_provenance_t eight = prov_with_macros(8);
    n = envelope_provenance_part(&eight, TRACE_ROW, part, sizeof(part));
    CHECK(n == (int)strlen(part));
    CHECK(n < (int)sizeof(part));
    CHECK(strncmp(part, "\"workbook_version_id\":", 22) == 0);
    CHECK(strstr(part, "\"macros\":[") != NULL);
    CHECK(strstr(part, "macro_00000007\"}],") != NULL);
    CHECK(macro_entries(part) == 8);

    /* macros without a workbook id: the macros key stands alone */
    schedule_provenance_t macros_only = prov_with_macros(2);
    macros_only.workbook_version_id[0] = '\0';
    n = envelope_provenance_part(&macros_only, TRACE_ROW, part, sizeof(part));
    CHECK(n == (int)strlen(part));
    CHECK(strncmp(part, "\"macros\":[", 10) == 0);

    /* JSON-unsafe workbook id → the key drops, macros survive */
    schedule_provenance_t bad_wb = prov_with_macros(1);
    memset(bad_wb.workbook_version_id, 0, sizeof(bad_wb.workbook_version_id));
    snprintf(bad_wb.workbook_version_id, sizeof(bad_wb.workbook_version_id),
             "bad\"id");
    n = envelope_provenance_part(&bad_wb, TRACE_ROW, part, sizeof(part));
    CHECK(strstr(part, "workbook_version_id") == NULL);
    CHECK(strncmp(part, "\"macros\":[", 10) == 0);

    /* JSON-unsafe macro field → the whole macros key drops (all-or-nothing),
     * the workbook key still stands */
    schedule_provenance_t bad_macro = prov_with_macros(1);
    memset(bad_macro.macros[0].name, 0, sizeof(bad_macro.macros[0].name));
    snprintf(bad_macro.macros[0].name, sizeof(bad_macro.macros[0].name),
             "has space");
    n = envelope_provenance_part(&bad_macro, TRACE_ROW, part, sizeof(part));
    CHECK(strcmp(part, "\"workbook_version_id\":\"" WB_ID "\",") == 0);

    /* … but an unsafe macro that this row FILTERS OUT must not drop the key:
     * all-or-nothing survives only among the macros that actually render */
    schedule_provenance_t bad_filtered = prov_with_macros(2);
    memset(bad_filtered.macros[1].name, 0, sizeof(bad_filtered.macros[1].name));
    snprintf(bad_filtered.macros[1].name, sizeof(bad_filtered.macros[1].name),
             "has space");
    add_cond(&bad_filtered, 1, SCHED_PROV_COND_FIELD_SCHEMA,
             SCHED_PROV_COND_OP_EQ, "ambit.spectrum/1");
    n = envelope_provenance_part(&bad_filtered, TRACE_ROW, part, sizeof(part));
    CHECK(macro_entries(part) == 1);
    CHECK(strstr(part, "has space") == NULL);
    CHECK(strstr(part, "macro_00000000") != NULL);

    /* a corrupt count → nothing is emitted */
    schedule_provenance_t corrupt = prov_with_macros(1);
    corrupt.macro_count = SCHEDULE_PROVENANCE_MAX_MACROS + 1;
    CHECK(envelope_provenance_part(&corrupt, TRACE_ROW, part, sizeof(part)) == 0);
    CHECK(part[0] == '\0');

    /* undersized output buffer → empty, never a truncated (malformed) splice */
    char tiny[64];
    CHECK(envelope_provenance_part(&one, TRACE_ROW, tiny, sizeof(tiny)) == 0);
    CHECK(tiny[0] == '\0');
    CHECK(envelope_provenance_part(&one, TRACE_ROW, NULL, 0) == 0);

    /* ── per-row routing: one header, one macro per family ──────────────
     * This is the whole point of the feature: openJII runs, per published
     * row, exactly the macros that row's envelope lists, so a telemetry row
     * must carry the telemetry macro ALONE. */
    schedule_provenance_t routed = prov_routed_by_schema();

    n = envelope_provenance_part(&routed, TRACE_ROW, part, sizeof(part));
    CHECK(macro_entries(part) == 1);
    CHECK(strstr(part, "\"name\":\"ambyte-trace\"") != NULL);
    CHECK(strstr(part, "ambyte-spectrum") == NULL);
    CHECK(strstr(part, "ambyte-telemetry") == NULL);
    CHECK(n == (int)strlen(part));

    n = envelope_provenance_part(&routed, SPECTRUM_ROW, part, sizeof(part));
    CHECK(macro_entries(part) == 1);
    CHECK(strstr(part, "\"name\":\"ambyte-spectrum\"") != NULL);

    n = envelope_provenance_part(&routed, TELEMETRY_ROW, part, sizeof(part));
    CHECK(macro_entries(part) == 1);
    CHECK(strstr(part, "\"name\":\"ambyte-telemetry\"") != NULL);

    /* an unmatched family (a v2 backlog row carries no `schema` at all):
     * the macros key is omitted entirely, the workbook key still stands */
    n = envelope_provenance_part(&routed, V2_ROW, part, sizeof(part));
    CHECK(strcmp(part, "\"workbook_version_id\":\"" WB_ID "\",") == 0);
    CHECK(n == (int)strlen(part));

    /* the legacy idiom: schema neq ×4 (exactly the condition cap) matches the
     * v2 row and nothing canonical — absent-field neq is TRUE, which is what
     * makes "everything the four v3 families do not cover" expressible */
    schedule_provenance_t legacy = prov_with_macros(1);
    name_macro(&legacy, 0, "ambyte-legacy", "macro_legacyv2");
    add_cond(&legacy, 0, SCHED_PROV_COND_FIELD_SCHEMA, SCHED_PROV_COND_OP_NEQ,
             "ambit.trace/3");
    add_cond(&legacy, 0, SCHED_PROV_COND_FIELD_SCHEMA, SCHED_PROV_COND_OP_NEQ,
             "ambit.spectrum/1");
    add_cond(&legacy, 0, SCHED_PROV_COND_FIELD_SCHEMA, SCHED_PROV_COND_OP_NEQ,
             "ambyte.telemetry/1");
    add_cond(&legacy, 0, SCHED_PROV_COND_FIELD_SCHEMA, SCHED_PROV_COND_OP_NEQ,
             "ambit.device/1");
    CHECK(legacy.macros[0].cond_count == SCHEDULE_PROVENANCE_MAX_MACRO_CONDS);
    envelope_provenance_part(&legacy, V2_ROW, part, sizeof(part));
    CHECK(macro_entries(part) == 1);
    envelope_provenance_part(&legacy, TRACE_ROW, part, sizeof(part));
    CHECK(macro_entries(part) == 0);
    envelope_provenance_part(&legacy, TELEMETRY_ROW, part, sizeof(part));
    CHECK(macro_entries(part) == 0);

    /* multi-condition AND over non-schema fields, including the nested
     * protocol object (the top-level "tag" must not answer protocol.tag) */
    schedule_provenance_t anded = prov_with_macros(1);
    name_macro(&anded, 0, "ambyte-edge", "macro_edge0001");
    add_cond(&anded, 0, SCHED_PROV_COND_FIELD_CHANNEL, SCHED_PROV_COND_OP_EQ,
             "uart_0");
    add_cond(&anded, 0, SCHED_PROV_COND_FIELD_PROTOCOL_TAG,
             SCHED_PROV_COND_OP_EQ, "edge");
    envelope_provenance_part(&anded, TRACE_ROW, part, sizeof(part));
    CHECK(macro_entries(part) == 1);
    /* the spectrum row matches channel but has no protocol object: AND fails */
    envelope_provenance_part(&anded, SPECTRUM_ROW, part, sizeof(part));
    CHECK(macro_entries(part) == 0);
    /* the v2 row's top-level "tag" is MEASUREMENT, not "edge", and must not be
     * mistaken for protocol.tag anyway */
    envelope_provenance_part(&anded, V2_ROW, part, sizeof(part));
    CHECK(macro_entries(part) == 0);

    /* ── the three production format strings, rendered for Python ── */
    char env[4096];
    schedule_provenance_t two = prov_with_macros(2);
    char wbpart[1440];
    envelope_provenance_part(&two, TRACE_ROW, wbpart, sizeof(wbpart));

    snprintf(env, sizeof(env), DC_V3_EVENT_ENVELOPE_FMT,
             "{\"schema\":\"ambit.trace/3\",\"measure_id\":26337}",
             "2026-08-05T19:26:00Z",
             "\"device_battery\":3.912,", "\"timezone\":\"Europe/Amsterdam\",",
             wbpart,
             "28:37:2F:FF:E7:04", "AmbyteOnAir", "V003", "1.6.6");
    printf("V3_FULL=%s\n", env);

    snprintf(env, sizeof(env), DC_V3_EVENT_ENVELOPE_FMT,
             "{\"schema\":\"ambit.trace/3\",\"measure_id\":26337}",
             "2026-08-05T19:26:00Z",
             "\"device_battery\":3.912,", "\"timezone\":\"Europe/Amsterdam\",",
             "",
             "28:37:2F:FF:E7:04", "AmbyteOnAir", "V003", "1.6.6");
    printf("V3_NOPROV=%s\n", env);

    snprintf(env, sizeof(env), DC_EVENT_ENVELOPE_FMT,
             26337LL, 1785948360000LL, 1785948364000LL,
             "2026-08-05T21:26:00+02:00", "2026-08-05T19:26:54Z",
             "\"uart_0\"", "\"AmbitV003\"", "\"arrun 1,0,0,0\"", "MEASUREMENT",
             "{\"protocol\":\"SS\"}", "{\"s_630\":[1,2,3]}",
             "2026-08-05T19:26:00Z",
             "\"device_battery\":3.912,", "\"timezone\":\"Europe/Amsterdam\",",
             wbpart,
             "28:37:2F:FF:E7:04", "AmbyteOnAir", "V003", "1.6.6");
    printf("V2_FULL=%s\n", env);

    snprintf(env, sizeof(env), DC_V3_GZ_EVENT_ENVELOPE_FMT,
             "H4sIAAAAAAAAA6tWqq5QAAIAAP//AwA=",
             "2026-08-05T19:26:00Z",
             "\"device_battery\":3.912,", "\"timezone\":\"Europe/Amsterdam\",",
             wbpart,
             "28:37:2F:FF:E7:04", "AmbyteOnAir", "V003", "1.6.6");
    printf("GZ_FULL=%s\n", env);

    /* Two rows, ONE routed header: the wire evidence that a telemetry row and
     * a trace row published by the same device carry different macro lists. */
    envelope_provenance_part(&routed, TRACE_ROW, wbpart, sizeof(wbpart));
    snprintf(env, sizeof(env), DC_V3_EVENT_ENVELOPE_FMT,
             TRACE_ROW, "2026-08-05T19:26:00Z",
             "\"device_battery\":3.912,", "\"timezone\":\"Europe/Amsterdam\",",
             wbpart,
             "28:37:2F:FF:E7:04", "AmbyteOnAir", "V003", "1.6.6");
    printf("V3_ROUTED_TRACE=%s\n", env);

    envelope_provenance_part(&routed, TELEMETRY_ROW, wbpart, sizeof(wbpart));
    snprintf(env, sizeof(env), DC_V3_EVENT_ENVELOPE_FMT,
             TELEMETRY_ROW, "2026-08-05T19:26:00Z",
             "\"device_battery\":3.912,", "\"timezone\":\"Europe/Amsterdam\",",
             wbpart,
             "28:37:2F:FF:E7:04", "AmbyteOnAir", "V003", "1.6.6");
    printf("V3_ROUTED_TELEMETRY=%s\n", env);

    envelope_provenance_part(&routed, V2_ROW, wbpart, sizeof(wbpart));
    snprintf(env, sizeof(env), DC_EVENT_ENVELOPE_FMT,
             26339LL, 1785948360000LL, 1785948364000LL,
             "2026-08-05T21:26:00+02:00", "2026-08-05T19:26:54Z",
             "\"uart_1\"", "\"AmbitV003\"", "\"arrun 1,0,0,0\"", "MEASUREMENT",
             "null", "{\"s_630\":[1,2,3]}",
             "2026-08-05T19:26:00Z",
             "\"device_battery\":3.912,", "\"timezone\":\"Europe/Amsterdam\",",
             wbpart,
             "28:37:2F:FF:E7:04", "AmbyteOnAir", "V003", "1.6.6");
    printf("V2_ROUTED_UNMATCHED=%s\n", env);

    printf("ENVELOPE_PROVENANCE_HOST_OK %d checks\n", s_checks);
    return s_fails == 0 ? 0 : 1;
}
