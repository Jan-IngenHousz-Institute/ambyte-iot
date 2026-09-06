/*
 * payload_scalar_host.c — unit runner for the payload scalar scanner,
 * compiled by tests/test_payload_scalar.py (pattern: tests/
 * payload_v3_host.c). Pure host C11. The fixtures are REAL payload_v3 builder
 * output (trace + spectrum + telemetry), not hand-written approximations, so
 * the scanner is exercised against the exact bytes the publish path stores.
 *
 * Prints "PAYLOAD_SCALAR_HOST_OK <checks>" and exits 0 when all checks pass;
 * before that it prints the fixtures as KEY=<json> lines for the Python side
 * to json-parse.
 */

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "payload_scalar.h"
#include "payload_v3.h"

static int s_checks, s_fails;
#define CHECK(cond)                                                     \
    do {                                                                \
        s_checks++;                                                     \
        if (!(cond)) {                                                  \
            s_fails++;                                                  \
            printf("FAIL %d: %s\n", __LINE__, #cond);                   \
        }                                                               \
    } while (0)

/* true + the expected string */
#define CHECK_SCALAR(json, path, want)                                  \
    do {                                                                \
        char v[48];                                                     \
        s_checks++;                                                     \
        if (!payload_scalar((json), (path), v, sizeof(v)) ||            \
            strcmp(v, (want)) != 0) {                                   \
            s_fails++;                                                  \
            printf("FAIL %d: %s → %s\n", __LINE__, (path),              \
                   v);                                                  \
        }                                                               \
    } while (0)

#define CHECK_ABSENT(json, path)                                        \
    do {                                                                \
        char v[48];                                                     \
        s_checks++;                                                     \
        if (payload_scalar((json), (path), v, sizeof(v))) {             \
            s_fails++;                                                  \
            printf("FAIL %d: %s unexpectedly found (%s)\n",             \
                   __LINE__, (path), v);                                \
        }                                                               \
    } while (0)

int main(void)
{
    char error[128];

    /* trace fixture: the tagged variant, so protocol.name AND protocol.tag
     * are both present (see print_tagged_trace_fixture in payload_v3_host.c) */
    static char trace[4096];
    uint32_t temp[] = {2500, 2510};
    uint32_t counts[] = {10, 11, 12, 13};
    uint32_t timing[] = {1000, 2001000};
    payload_v3_array_t arrays[] = {{0, 2, temp}, {1, 4, counts}, {7, 2, timing}};
    payload_v3_segment_t segment = {2, 4, 2, 7, 1};
    payload_v3_trace_input_t trace_in = {
        .measure_id = 7,
        .channel = "uart_0",
        .device = "AmbitV003",
        .sensor_id = "10:91:A8:4F:4F:C0",
        .start_utc_ms = 100,
        .end_utc_ms = 2200,
        .protocol_name = "MPF",
        .protocol_tag = "edge",
        .protocol_cmd = "arrun edge",
        .segments = &segment,
        .segment_count = 1,
        .calibration_present = true,
        .cal_version = 0x439a0ac8,
        .tick_factor = 0.854,
        .arrays = arrays,
        .array_count = 3,
    };
    assert(payload_v3_build_trace(trace, sizeof trace, &trace_in, error, sizeof error));
    printf("TRACE=%s\n", trace);

    CHECK_SCALAR(trace, "schema", "ambit.trace/3");
    CHECK_SCALAR(trace, "tag", "MEASUREMENT");
    CHECK_SCALAR(trace, "channel", "uart_0");
    CHECK_SCALAR(trace, "device", "AmbitV003");
    CHECK_SCALAR(trace, "sensor_id", "10:91:A8:4F:4F:C0");
    CHECK_SCALAR(trace, "protocol.name", "MPF");
    CHECK_SCALAR(trace, "protocol.tag", "edge");

    /* spectrum fixture: no protocol object at all */
    static char spectrum[512];
    payload_v3_spectrum_input_t spec_in = {
        .measure_id = 65928,
        .channel = "uart_0",
        .device = "AD81",
        .sensor_id = "3C:DC:75:0D:FD:20",
        .start_utc_ms = 1788718542591LL,
        .end_utc_ms = 1788718542944LL,
        .calibration_present = true,
        .cal_version = 0xc96cda1bU,
        .par = 12.75,
        .spectrum = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9},
    };
    assert(payload_v3_build_spectrum(spectrum, sizeof spectrum, &spec_in,
                                     error, sizeof error));
    printf("SPECTRUM=%s\n", spectrum);

    CHECK_SCALAR(spectrum, "schema", "ambit.spectrum/1");
    CHECK_SCALAR(spectrum, "device", "AD81");
    CHECK_ABSENT(spectrum, "protocol.name");
    CHECK_ABSENT(spectrum, "protocol.tag");

    /* telemetry fixture: channel/sensor_id exist ONLY nested inside
     * attached_sensors — the depth-exact match must NOT answer top-level
     * queries with them */
    static char telemetry[4096];
    payload_v3_telemetry_input_t tele_in = {
        .measure_id = 26338,
        .device = "28:37:2F:FF:E7:04",
        .observed_utc_ms = 1785965213985LL,
        .attached_count = 1,
        .attached = {{
            .present = true,
            .channel = "uart_0",
            .sensor_id = "10:91:A8:4F:4F:D4",
        }},
    };
    assert(payload_v3_build_telemetry(telemetry, sizeof telemetry, &tele_in,
                                      error, sizeof error));
    printf("TELEMETRY=%s\n", telemetry);

    CHECK_SCALAR(telemetry, "schema", "ambyte.telemetry/1");
    CHECK_SCALAR(telemetry, "tag", "TELEMETRY");
    CHECK_SCALAR(telemetry, "device", "28:37:2F:FF:E7:04");
    CHECK_ABSENT(telemetry, "channel");
    CHECK_ABSENT(telemetry, "sensor_id");

    /* missing key / non-string value / malformed key paths */
    CHECK_ABSENT(trace, "nope");
    CHECK_ABSENT(trace, "measure_id");      /* number, not a string */
    CHECK_ABSENT(trace, "protocol.segments"); /* array, not a string */
    CHECK_ABSENT(trace, "protocol.cmd.x");  /* deeper than one level */
    CHECK_ABSENT(trace, ".tag");
    CHECK_ABSENT(trace, "protocol.");
    CHECK_ABSENT(trace, "");

    /* truncated JSON: cut inside the schema value and past the protocol
     * object — both must fail, never return a partial value */
    {
        char cut[64];
        snprintf(cut, sizeof cut, "%.20s", trace); /* mid "ambit.trace/3" */
        CHECK_ABSENT(cut, "schema");
        size_t proto_off = (size_t)(strstr(trace, "\"protocol\"") - trace);
        static char cut2[4096];
        snprintf(cut2, proto_off + 20, "%s", trace); /* inside protocol */
        CHECK_ABSENT(cut2, "protocol.name");
    }

    /* value ≥ cap: "AmbitV003" needs 10 bytes; cap 4 and exact-fit boundary */
    {
        char v[10];
        s_checks++;
        if (payload_scalar(trace, "device", v, 4)) {
            s_fails++;
            printf("FAIL %d: oversized value accepted\n", __LINE__);
        }
        CHECK(payload_scalar(trace, "device", v, sizeof v) &&
              strcmp(v, "AmbitV003") == 0);
    }

    /* v2-shaped rows: a bare quantities object carries none of the routing
     * fields (schema absent — the absent-field semantics the legacy idiom
     * relies on); a v2 object that DOES carry tag/channel/device at its top
     * level is addressable there */
    const char *v2_data = "{\"env\":[25.00,25.10],\"s_630\":[10,11,12,13]}";
    CHECK_ABSENT(v2_data, "schema");
    CHECK_ABSENT(v2_data, "tag");
    CHECK_ABSENT(v2_data, "channel");
    const char *v2_full = "{\"v\":2,\"tag\":\"MEASUREMENT\",\"channel\":\"uart_1\","
                          "\"device\":\"AmbitV003\",\"data\":{\"s_630\":[1]}}";
    CHECK_ABSENT(v2_full, "schema");
    CHECK_SCALAR(v2_full, "tag", "MEASUREMENT");
    CHECK_SCALAR(v2_full, "channel", "uart_1");
    CHECK_SCALAR(v2_full, "device", "AmbitV003");

    /* escaped quotes inside an earlier string value must not wedge the scan
     * or fake a key boundary */
    const char *escaped = "{\"schema\":\"ambit.trace/3\","
                          "\"note\":\"say \\\"schema\\\": \\\"x\\\" here\","
                          "\"tag\":\"DIAGNOSTIC\"}";
    CHECK_SCALAR(escaped, "schema", "ambit.trace/3");
    CHECK_SCALAR(escaped, "tag", "DIAGNOSTIC");

    /* NULL / empty inputs are plain "absent" */
    {
        char v[48];
        CHECK(!payload_scalar(NULL, "schema", v, sizeof v));
        CHECK(!payload_scalar(trace, "schema", NULL, 0));
        CHECK(!payload_scalar(trace, "schema", v, 0));
        CHECK(!payload_scalar("not json", "schema", v, sizeof v));
        CHECK(!payload_scalar("[1,2]", "schema", v, sizeof v));
    }

    printf("PAYLOAD_SCALAR_HOST_OK %d checks\n", s_checks);
    return s_fails == 0 ? 0 : 1;
}
