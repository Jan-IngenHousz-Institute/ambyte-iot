/*
 * envelope_provenance_host.c — unit runner for the publish envelope's
 * workbook-provenance part and the three production envelope format strings,
 * compiled by tests/test_envelope_provenance.py (pattern: tests/
 * payload_v3_host.c). Pure host C11; the code under test is the exact
 * production envelope_provenance.c + the real DC_*_ENVELOPE_FMT macros, so a
 * malformed C format key cannot slip past the Python-side reconstructions.
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

int main(void)
{
    char part[1440];

    /* unset everything → empty part (envelope byte-identical to today) */
    schedule_provenance_t empty;
    memset(&empty, 0, sizeof(empty));
    CHECK(envelope_provenance_part(&empty, part, sizeof(part)) == 0);
    CHECK(part[0] == '\0');
    CHECK(envelope_provenance_part(NULL, part, sizeof(part)) == 0);

    /* workbook only */
    schedule_provenance_t wb_only;
    memset(&wb_only, 0, sizeof(wb_only));
    snprintf(wb_only.workbook_version_id, sizeof(wb_only.workbook_version_id),
             "%s", WB_ID);
    int n = envelope_provenance_part(&wb_only, part, sizeof(part));
    CHECK(n == (int)strlen(part));
    CHECK(strcmp(part, "\"workbook_version_id\":\"" WB_ID "\",") == 0);

    /* workbook + one macro: exact bytes */
    schedule_provenance_t one = prov_with_macros(1);
    n = envelope_provenance_part(&one, part, sizeof(part));
    CHECK(n == (int)strlen(part));
    CHECK(strcmp(part,
                 "\"workbook_version_id\":\"" WB_ID "\","
                 "\"macros\":[{\"id\":\"47b03f78-a0d4-4b1d-bcd6-6e0b7d470040\","
                 "\"name\":\"ambyte-trace-0\",\"filename\":\"macro_00000000\"}],") == 0);

    /* eight macros: bounded, valid, count preserved */
    schedule_provenance_t eight = prov_with_macros(8);
    n = envelope_provenance_part(&eight, part, sizeof(part));
    CHECK(n == (int)strlen(part));
    CHECK(n < (int)sizeof(part));
    CHECK(strncmp(part, "\"workbook_version_id\":", 22) == 0);
    CHECK(strstr(part, "\"macros\":[") != NULL);
    CHECK(strstr(part, "macro_00000007\"}],") != NULL);

    /* macros without a workbook id: the macros key stands alone */
    schedule_provenance_t macros_only = prov_with_macros(2);
    macros_only.workbook_version_id[0] = '\0';
    n = envelope_provenance_part(&macros_only, part, sizeof(part));
    CHECK(n == (int)strlen(part));
    CHECK(strncmp(part, "\"macros\":[", 10) == 0);

    /* JSON-unsafe workbook id → the key drops, macros survive */
    schedule_provenance_t bad_wb = prov_with_macros(1);
    memset(bad_wb.workbook_version_id, 0, sizeof(bad_wb.workbook_version_id));
    snprintf(bad_wb.workbook_version_id, sizeof(bad_wb.workbook_version_id),
             "bad\"id");
    n = envelope_provenance_part(&bad_wb, part, sizeof(part));
    CHECK(strstr(part, "workbook_version_id") == NULL);
    CHECK(strncmp(part, "\"macros\":[", 10) == 0);

    /* JSON-unsafe macro field → the whole macros key drops (all-or-nothing),
     * the workbook key still stands */
    schedule_provenance_t bad_macro = prov_with_macros(1);
    memset(bad_macro.macros[0].name, 0, sizeof(bad_macro.macros[0].name));
    snprintf(bad_macro.macros[0].name, sizeof(bad_macro.macros[0].name),
             "has space");
    n = envelope_provenance_part(&bad_macro, part, sizeof(part));
    CHECK(strcmp(part, "\"workbook_version_id\":\"" WB_ID "\",") == 0);

    /* a corrupt count → nothing is emitted */
    schedule_provenance_t corrupt = prov_with_macros(1);
    corrupt.macro_count = SCHEDULE_PROVENANCE_MAX_MACROS + 1;
    CHECK(envelope_provenance_part(&corrupt, part, sizeof(part)) == 0);
    CHECK(part[0] == '\0');

    /* undersized output buffer → empty, never a truncated (malformed) splice */
    char tiny[64];
    CHECK(envelope_provenance_part(&one, tiny, sizeof(tiny)) == 0);
    CHECK(tiny[0] == '\0');
    CHECK(envelope_provenance_part(&one, NULL, 0) == 0);

    /* ── the three production format strings, rendered for Python ── */
    char env[4096];
    schedule_provenance_t two = prov_with_macros(2);
    char wbpart[1440];
    envelope_provenance_part(&two, wbpart, sizeof(wbpart));

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

    printf("ENVELOPE_PROVENANCE_HOST_OK %d checks\n", s_checks);
    return s_fails == 0 ? 0 : 1;
}
