#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "payload_v3.h"

static void fill_values(uint32_t *values, size_t count, uint32_t base)
{
    for (size_t i = 0; i < count; ++i) values[i] = base + (uint32_t)i;
}

static void print_trace_fixtures(void)
{
    uint32_t temp[] = {2529, 2460, 2455, 2450, 2445, 2440};
    uint32_t offsets[] = {0, 8600, 17100, 25700, 34200, 42800};
    uint32_t timing[] = {0xffff0000U, 0x02ffea20U}; /* wrap-safe 50,391.584 ms */
    uint32_t values[4][59];
    for (size_t i = 0; i < 4; ++i) fill_values(values[i], 59, 100U + (uint32_t)i * 1000U);
    values[0][58] = 70000U; /* contract clamp */

    payload_v3_array_t arrays[] = {
        {0, 6, temp}, {1, 59, values[0]}, {2, 59, values[1]},
        {3, 59, values[2]}, {4, 59, values[3]},
        {7, 2, timing}, {8, 6, offsets},
    };
    payload_v3_segment_t segment = {2, 59, 1, 0, 1};
    payload_v3_trace_input_t input = {
        .measure_id = 26337,
        .channel = "uart_1",
        .device = "AmbitV003",
        .sensor_id = "10:91:A8:4F:4F:D4",
        .start_utc_ms = 1785965160359LL,
        .end_utc_ms = 1785965213985LL,
        .protocol_name = "SS",
        .protocol_id = NULL, /* current Ambyte producer has no id delivery path */
        .protocol_cmd = "arrun 1,0,2,0,0,59,0,1,0,1",
        .segments = &segment,
        .segment_count = 1,
        .calibration_present = true,
        .cal_version = 0x6a4356a8U,
        .tick_factor = 0.854,
        .gains_present = true,
        .gains = {1, 1, 2, 2, 1, 1},
        .currents_present = true,
        .currents = {55, 0, 10},
        .arrays = arrays,
        .array_count = sizeof arrays / sizeof arrays[0],
    };
    char output[16384], error[128];
    assert(payload_v3_build_trace(output, sizeof output, &input, error, sizeof error));
    printf("TRACE_IDX8=%s\n", output);

    /* Force the same production input through the lossless v2 builder without
     * changing any wire values. Missing identity is representative of the
     * historical v2 producer and keeps the size comparison apples-to-apples. */
    char legacy_metadata[2048];
    const size_t idx8_array_count = input.array_count;
    input.array_count--; /* historical v2 AMBIT firmware had no additive idx 8 */
    input.sensor_id = NULL;
    input.tick_factor = PAYLOAD_V3_TICK_FACTOR_MAX + 1.0;
    assert(payload_v3_build_trace_lossless(
               output, sizeof output, legacy_metadata, sizeof legacy_metadata,
               &input, error, sizeof error) == PAYLOAD_TRACE_ROUTE_V2);
    printf("TRACE_V2_DATA=%s\n", output);
    printf("TRACE_V2_METADATA=%s\n", legacy_metadata);
    input.sensor_id = "10:91:A8:4F:4F:D4";
    input.tick_factor = 0.854;
    input.array_count = idx8_array_count;

    input.array_count--;
    assert(payload_v3_build_trace(output, sizeof output, &input, error, sizeof error));
    printf("TRACE_FALLBACK=%s\n", output);

    uint32_t mixed_values[] = {1, 2, 3, 4, 5};
    uint32_t mixed_temp[] = {2500};
    uint32_t mixed_timing[] = {1000, 3417000};
    payload_v3_array_t mixed_arrays[] = {
        {0, 1, mixed_temp}, {1, 5, mixed_values}, {7, 2, mixed_timing},
    };
    payload_v3_segment_t mixed_segments[] = {
        {2, 3, 2, 0, 1}, {2, 2, 1, 0, 1},
    };
    input.protocol_name = NULL;
    input.protocol_id = NULL;
    input.protocol_cmd = "arrun mixed";
    input.segments = mixed_segments;
    input.segment_count = 2;
    input.arrays = mixed_arrays;
    input.array_count = 3;
    input.gains_present = false;
    input.currents_present = false;
    assert(payload_v3_build_trace(output, sizeof output, &input, error, sizeof error));
    printf("TRACE_MIXED=%s\n", output);

    uint32_t ambient_value[] = {42};
    uint32_t sub_timing[] = {0, 6832000};
    payload_v3_array_t sub_arrays[] = {
        {0, 1, mixed_temp}, {1, 8, values[0]}, {3, 1, ambient_value},
        {7, 2, sub_timing},
    };
    payload_v3_segment_t sub_segment = {2, 8, 1, 0, 2};
    input.protocol_cmd = "arrun subsample";
    input.segments = &sub_segment;
    input.segment_count = 1;
    input.arrays = sub_arrays;
    input.array_count = 4;
    assert(payload_v3_build_trace(output, sizeof output, &input, error, sizeof error));
    printf("TRACE_SUBSAMPLE=%s\n", output);
}

static void test_lossless_trace_routes(void)
{
    uint32_t temp[] = {2500, 2510};
    uint32_t counts[] = {10, 11, 12, 13};
    uint32_t timing[] = {1000, 2001000};
    uint32_t offsets[] = {0, 2000};
    payload_v3_array_t arrays[] = {
        {0, 2, temp}, {1, 4, counts}, {7, 2, timing}, {8, 2, offsets},
    };
    payload_v3_segment_t segment = {2, 4, 2, 7, 1};
    payload_v3_trace_input_t input = {
        .measure_id = 1,
        .channel = "uart_0",
        .device = "AmbitV003",
        .sensor_id = "10:91:A8:4F:4F:C0",
        .start_utc_ms = 100,
        .end_utc_ms = 2200,
        .protocol_name = "exact",
        .protocol_id = "pid",
        .protocol_cmd = "arrun exact",
        .segments = &segment,
        .segment_count = 1,
        .calibration_present = true,
        .cal_version = 0x439a0ac8,
        .tick_factor = 0.854,
        .arrays = arrays,
        .array_count = 4,
    };
    char out[4096], metadata[1536], error[128];
    assert(payload_v3_build_trace_lossless(out, sizeof out, metadata, sizeof metadata,
                                            &input, error, sizeof error) ==
           PAYLOAD_TRACE_ROUTE_V3);
    assert(strstr(out, "\"schema\":\"ambit.trace/3\"") != NULL);
    assert(metadata[0] == '\0');

    input.calibration_present = false;
    assert(payload_v3_build_trace_lossless(out, sizeof out, metadata, sizeof metadata,
                                            &input, error, sizeof error) ==
           PAYLOAD_TRACE_ROUTE_V2);
    assert(strcmp(out, "{\"env\":[25.00,25.10],\"s_630\":[10,11,12,13],"
                       "\"timing\":[1000,2001000],\"arr8\":[0,2000]}") == 0);
    assert(strstr(metadata, "\"cal_version\":null") != NULL);
    assert(strstr(metadata, "\"sensor_id\":\"10:91:A8:4F:4F:C0\"") != NULL);

    input.calibration_present = true;
    input.sensor_id = NULL;
    assert(payload_v3_build_trace_lossless(out, sizeof out, metadata, sizeof metadata,
                                            &input, error, sizeof error) ==
           PAYLOAD_TRACE_ROUTE_V2);
    assert(strstr(metadata, "sensor_id") == NULL);
    input.sensor_id = "10:91:A8:4F:4F:C0";

    arrays[1].length = 3; /* interrupted/short result uses actual returned length */
    assert(payload_v3_build_trace_lossless(out, sizeof out, metadata, sizeof metadata,
                                            &input, error, sizeof error) ==
           PAYLOAD_TRACE_ROUTE_V3);
    assert(strstr(out, "\"v\":[10,11,12]") != NULL);

    arrays[1].length = 4;
    segment.pulses = 3; /* known series longer than protocol: retain via v2 */
    assert(payload_v3_build_trace_lossless(out, sizeof out, metadata, sizeof metadata,
                                            &input, error, sizeof error) ==
           PAYLOAD_TRACE_ROUTE_V2);
    assert(strstr(out, "\"s_630\":[10,11,12,13]") != NULL);
    segment.pulses = 4;

    arrays[1].index = 9; /* forward-compatible unknown series on main clock */
    assert(payload_v3_build_trace_lossless(out, sizeof out, metadata, sizeof metadata,
                                            &input, error, sizeof error) ==
           PAYLOAD_TRACE_ROUTE_V3);
    assert(strstr(out, "\"arr9\":{\"u\":\"count\"") != NULL);
    arrays[1].index = 1;

    input.array_count = 2; /* missing calibration timing is a permanent v2 fallback */
    assert(payload_v3_build_trace_lossless(out, sizeof out, metadata, sizeof metadata,
                                            &input, error, sizeof error) ==
           PAYLOAD_TRACE_ROUTE_V2);
    assert(strstr(out, "\"s_630\":[10,11,12,13]") != NULL);
    assert(strstr(metadata, "\"sensor_id\":\"10:91:A8:4F:4F:C0\"") != NULL);
    arrays[2].length = 1;
    input.array_count = 3;
    assert(payload_v3_build_trace_lossless(out, sizeof out, metadata, sizeof metadata,
                                            &input, error, sizeof error) ==
           PAYLOAD_TRACE_ROUTE_V2);
    assert(strstr(metadata, "\"sensor_id\":\"10:91:A8:4F:4F:C0\"") != NULL);
    arrays[2].length = 2;
    input.array_count = 4;

    input.tick_factor = PAYLOAD_V3_TICK_FACTOR_MAX + 0.001;
    assert(payload_v3_build_trace_lossless(out, sizeof out, metadata, sizeof metadata,
                                            &input, error, sizeof error) ==
           PAYLOAD_TRACE_ROUTE_V2);
    input.tick_factor = 0.854;

    payload_v3_segment_t max_segments[PAYLOAD_V3_MAX_SEGMENTS];
    for (size_t i = 0; i < PAYLOAD_V3_MAX_SEGMENTS; ++i)
        max_segments[i] = (payload_v3_segment_t){2, 65535, 65535, 255, 1};
    char max_protocol[256], max_protocol_id[256];
    memset(max_protocol, 'p', sizeof max_protocol - 1U);
    memset(max_protocol_id, 'i', sizeof max_protocol_id - 1U);
    max_protocol[sizeof max_protocol - 1U] = '\0';
    max_protocol_id[sizeof max_protocol_id - 1U] = '\0';
    input.segments = max_segments;
    input.segment_count = PAYLOAD_V3_MAX_SEGMENTS;
    input.protocol_name = max_protocol;
    input.protocol_id = max_protocol_id;
    input.tick_factor = PAYLOAD_V3_TICK_FACTOR_MAX + 0.001;
    assert(payload_v3_build_trace_lossless(out, sizeof out, metadata, sizeof metadata,
                                            &input, error, sizeof error) ==
           PAYLOAD_TRACE_ROUTE_V2);
    assert(metadata[0] != '\0');
    assert(strstr(metadata, "\"sensor_id\":\"10:91:A8:4F:4F:C0\"") != NULL);

    /* If the complete legacy metadata cannot fit, the lossless route must at
     * least retain the stable identity join key. It must not claim success with
     * empty metadata, and a buffer too small even for that key is an error. */
    char identity_only[64];
    assert(payload_v3_build_trace_lossless(out, sizeof out,
                                            identity_only, sizeof identity_only,
                                            &input, error, sizeof error) ==
           PAYLOAD_TRACE_ROUTE_V2);
    assert(strcmp(identity_only,
                  "{\"sensor_id\":\"10:91:A8:4F:4F:C0\"}") == 0);
    char identity_too_small[8];
    assert(payload_v3_build_trace_lossless(out, sizeof out,
                                            identity_too_small,
                                            sizeof identity_too_small,
                                            &input, error, sizeof error) ==
           PAYLOAD_TRACE_ROUTE_ERROR);

    assert(!payload_v3_can_fetch_retained(false, 1));
    assert(!payload_v3_can_fetch_retained(true, 0));
    assert(payload_v3_can_fetch_retained(true, 1));
}

static void print_telemetry_fixture(void)
{
    payload_v3_telemetry_input_t input = {
        .measure_id = 26338,
        .device = "28:37:2F:FF:E7:04",
        .observed_utc_ms = 1785965213985LL,
        .observations_valid = true,
        .air_temperature = 24.67,
        .relative_humidity = 61.2,
        .air_pressure = 101325.0,
        .connectivity_valid = true,
        .wifi = true,
        .provisioned = true,
        .publish_gate = true,
        .mqtt_reconnects = 2,
        .last_disc_reason = "",
        .conn_age_s = 86400,
        .pending = 3,
        .power_valid = true,
        .battery_v = 3.912,
        .input_v = 5.040,
        .system_v = 3.920,
        .input_ma = 518,
        .charge_ma = 297,
        .input_present = true,
        .charge_status = 2,
        .storage_db_valid = true,
        .db_online = true,
        .storage_sd_valid = true,
        .sd_free_kb = 1832448,
        .sd_skipped = 1,
        .sd_dropped = 2,
        .last_acked_id = 26335,
        .sd_io_lost = false,
        .runtime_valid = true,
        .uptime_s = 86400,
        .psram_free_kb = 7210,
        .psram_largest_kb = 7168,
        .psram_size_kb = 8192,
        .heap_dma_largest_kb = 32,
        .heap_int_free_kb = 121,
        .heap_int_largest_kb = 64,
        .wd_armed = true,
        .last_wd_reboot_reason = "",
        .clock_valid = true,
        .clock_source = "rtc",
        .clock_suspect = false,
        .software_valid = true,
        .firmware = "1.6.6",
        .script_valid = true,
        .script_sha256 = "7c222fb2927d828af22f592134e8932480637c0d2d88184a5be625042c22a6cd",
        .script_version = "1.0.0",
        .script_built_against_fw = "1.6.6",
        .script_installed_on_fw = "1.6.6",
        .script_metadata_verified = true,
        .attached_count = 1,
        .attached = {{
            .present = true,
            .channel = "uart_0",
            .sensor_id = "10:91:A8:4F:4F:D4",
            .firmware = "1.1.5",
            .hardware_revision = 1,
            .name = "AmbitV003",
            .cal_version_present = true,
            .cal_version = 0x6a4356a8,
        }},
    };
    char output[4096], error[128];
    assert(payload_v3_build_telemetry(output, sizeof output, &input, error, sizeof error));
    printf("TELEMETRY=%s\n", output);

    input.attached[0].cal_version_present = false;
    assert(payload_v3_build_telemetry(output, sizeof output, &input, error, sizeof error));
    printf("TELEMETRY_NOCAL=%s\n", output);

    input.storage_sd_valid = false;
    assert(payload_v3_build_telemetry(output, sizeof output, &input, error, sizeof error));
    printf("TELEMETRY_SD_FAIL=%s\n", output);

    input.air_temperature = NAN;
    assert(!payload_v3_build_telemetry(output, sizeof output, &input, error, sizeof error));

    memset(&input, 0, sizeof input);
    input.measure_id = 26340;
    input.device = "28:37:2F:FF:E7:04";
    input.observed_utc_ms = 1785965214999LL;
    assert(payload_v3_build_telemetry(output, sizeof output, &input, error, sizeof error));
    printf("TELEMETRY_EMPTY=%s\n", output);
}

static void print_device_fixture(void)
{
    payload_v3_device_input_t input = {
        .measure_id = 26339,
        .channel = "uart_0",
        .device = "AmbitV003",
        .observed_utc_ms = 1785965214102LL,
        .sensor_id = "10:91:A8:4F:4F:D4",
        .name = "AmbitV003",
        .firmware = "1.1.5",
        .hardware_revision = 1,
        .cal_version = 0x6a4356a8,
        .temp_offset = 0.0,
        .temp_slope = 1.0,
        .actinic_coef = 0.012345,
        .spec_coef = 1.0,
        .act = {12, 24, 36, 48, 60},
        .mlx_emissivity = 0.98,
        .sun_coef = 1.0,
        .tick_factor = 0.854,
    };
    for (size_t i = 0; i < 14; ++i) input.mlx_coef[i] = (int32_t)i + 1;
    for (size_t i = 0; i < 6; ++i) input.adpd[i] = (uint32_t)i + 100;
    char output[2048], error[128];
    assert(payload_v3_build_device(output, sizeof output, &input, error, sizeof error));
    printf("DEVICE=%s\n", output);

    assert(payload_v3_same_device_tuple("A", "1.0.0", 1, "A", "1.0.0", 1));
    assert(!payload_v3_same_device_tuple("A", "1.0.0", 1, "B", "1.0.0", 1));
    assert(!payload_v3_same_device_tuple("A", "1.0.0", 1, "A", "1.0.1", 1));
    assert(!payload_v3_same_device_tuple("A", "1.0.0", 1, "A", "1.0.0", 2));

    payload_v3_device_tuple_t tuple;
    assert(payload_v3_parse_device_tuple("AA:BB:CC:DD:EE:FF||1234abcd", &tuple));
    assert(tuple.firmware[0] == '\0' && tuple.cal_version == 0x1234abcdU);
    assert(!payload_v3_parse_device_tuple("AA:BB|1.0|xyz", &tuple));
    payload_v3_device_tuple_t tuples[PAYLOAD_V3_MAX_ATTACHED] = {
        {true, "A", "1", 1}, {true, "B", "1", 1},
        {true, "C", "1", 1}, {true, "D", "1", 1},
    };
    payload_v3_device_tuple_t candidate = {true, "E", "1", 1};
    const char *attached[PAYLOAD_V3_MAX_ATTACHED] = {"A", "B", "D", "E"};
    bool unchanged = true;
    assert(payload_v3_select_device_tuple_slot(tuples, &candidate, attached, 0,
                                                &unchanged) == 2U);
    assert(!unchanged);
    assert(payload_v3_is_canonical_object(output));
    assert(!payload_v3_is_canonical_object("{\"v\":2}"));

    input.tick_factor = NAN;
    assert(!payload_v3_build_device(output, sizeof output, &input, error, sizeof error));
    input.tick_factor = PAYLOAD_V3_TICK_FACTOR_MAX + 0.001;
    assert(!payload_v3_build_device(output, sizeof output, &input, error, sizeof error));
}

int main(void)
{
    print_trace_fixtures();
    test_lossless_trace_routes();
    print_telemetry_fixture();
    print_device_fixture();
    return 0;
}
