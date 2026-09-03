#include "payload_v3.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    bool ok;
} json_writer_t;

typedef enum {
    CLOCK_MAIN = 0,
    CLOCK_AMBIENT,
    CLOCK_REFLECTION,
} series_clock_t;

static void set_error(char *error, size_t cap, const char *message)
{
    if (error == NULL || cap == 0) return;
    snprintf(error, cap, "%s", message != NULL ? message : "payload build failed");
}

static void jw_init(json_writer_t *w, char *out, size_t cap)
{
    *w = (json_writer_t){.buf = out, .cap = cap, .len = 0, .ok = out != NULL && cap > 0};
    if (out != NULL && cap > 0) out[0] = '\0';
}

static void jw_append(json_writer_t *w, const char *fmt, ...)
{
    if (!w->ok) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(w->buf + w->len, w->cap - w->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= w->cap - w->len) {
        w->ok = false;
        w->buf[w->cap - 1U] = '\0';
        return;
    }
    w->len += (size_t)n;
}

static void jw_string(json_writer_t *w, const char *value)
{
    jw_append(w, "\"");
    if (!w->ok) return;
    const unsigned char *p = (const unsigned char *)(value != NULL ? value : "");
    for (; *p != '\0' && w->ok; ++p) {
        switch (*p) {
        case '\"': jw_append(w, "\\\""); break;
        case '\\': jw_append(w, "\\\\"); break;
        case '\b': jw_append(w, "\\b"); break;
        case '\f': jw_append(w, "\\f"); break;
        case '\n': jw_append(w, "\\n"); break;
        case '\r': jw_append(w, "\\r"); break;
        case '\t': jw_append(w, "\\t"); break;
        default:
            if (*p < 0x20U) jw_append(w, "\\u%04x", (unsigned)*p);
            else jw_append(w, "%c", (char)*p);
            break;
        }
    }
    jw_append(w, "\"");
}

static bool format_time(double value, char out[32])
{
    if (!isfinite(value)) return false;
    if (fabs(value) < 0.00005) value = 0.0;
    const int rendered = snprintf(out, 32, "%.4f", value);
    if (rendered < 0 || rendered >= 32) return false;
    size_t n = strlen(out);
    while (n > 0 && out[n - 1U] == '0') out[--n] = '\0';
    if (n > 0 && out[n - 1U] == '.') out[--n] = '\0';
    if (strcmp(out, "-0") == 0) strcpy(out, "0");
    return true;
}

static void jw_time(json_writer_t *w, double value)
{
    char rendered[32];
    if (!format_time(value, rendered)) {
        w->ok = false;
        return;
    }
    jw_append(w, "%s", rendered);
}

static bool valid_segment(const payload_v3_segment_t *s)
{
    return s != NULL && (s->type == 1U || s->type == 2U) && s->pulses > 0U &&
           s->freq > 0U && s->subsampling <= 2U;
}

static uint16_t segment_points(const payload_v3_segment_t *s, series_clock_t clock)
{
    if (!valid_segment(s)) return 0;
    if (clock == CLOCK_MAIN) return s->pulses;
    if (s->subsampling == 0U) return 0;
    if (clock == CLOCK_REFLECTION && s->type != 1U) return 0;
    return s->subsampling == 2U ? (uint16_t)(s->pulses / 8U) : s->pulses;
}

static double base_period(const payload_v3_segment_t *s, double tick_factor)
{
    return tick_factor / (double)s->freq;
}

static double point_period(const payload_v3_segment_t *s, series_clock_t clock,
                           double tick_factor)
{
    double period = base_period(s, tick_factor);
    if (clock != CLOCK_MAIN && s->subsampling == 2U) period *= 8.0;
    return period;
}

static double point_offset(const payload_v3_segment_t *s, series_clock_t clock,
                           double tick_factor)
{
    return (clock != CLOCK_MAIN && s->subsampling == 2U)
        ? 3.5 * base_period(s, tick_factor) : 0.0;
}

typedef struct {
    bool regular;
    double t0;
    double dt;
    size_t generated;
} time_model_t;

static time_model_t analyze_time(const payload_v3_trace_input_t *input,
                                 series_clock_t clock, size_t value_count)
{
    time_model_t model = {.regular = true};
    bool have_period = false;
    uint16_t first_freq = 0;
    uint8_t first_multiplier = 0;
    double segment_start = 0.0;

    for (size_t i = 0; i < input->segment_count; ++i) {
        const payload_v3_segment_t *s = &input->segments[i];
        if (!valid_segment(s)) continue;
        const uint16_t count = segment_points(s, clock);
        const double base = base_period(s, input->tick_factor);
        if (count != 0U) {
            const uint8_t multiplier = (clock != CLOCK_MAIN && s->subsampling == 2U) ? 8U : 1U;
            const double period = point_period(s, clock, input->tick_factor);
            if (!have_period) {
                model.dt = period;
                first_freq = s->freq;
                first_multiplier = multiplier;
                have_period = true;
            } else if (s->freq != first_freq || multiplier != first_multiplier) {
                model.regular = false;
            }
            for (uint16_t p = 0; p < count && model.generated < value_count; ++p) {
                const double t = segment_start + point_offset(s, clock, input->tick_factor) +
                                 (double)p * period;
                if (model.generated == 0U) model.t0 = t;
                else if (fabs(t - (model.t0 + (double)model.generated * model.dt)) > 0.00005)
                    model.regular = false;
                model.generated++;
            }
        }
        segment_start += (double)s->pulses * base;
        if (model.generated == value_count) break;
    }
    if (!have_period || model.generated != value_count) model.regular = false;
    return model;
}

static void write_explicit_times(json_writer_t *w,
                                 const payload_v3_trace_input_t *input,
                                 series_clock_t clock, size_t value_count)
{
    jw_append(w, "\"t\":[");
    size_t emitted = 0;
    double segment_start = 0.0;
    for (size_t i = 0; i < input->segment_count && emitted < value_count; ++i) {
        const payload_v3_segment_t *s = &input->segments[i];
        if (!valid_segment(s)) continue;
        const double base = base_period(s, input->tick_factor);
        const double first = point_offset(s, clock, input->tick_factor);
        const double period = point_period(s, clock, input->tick_factor);
        const uint16_t count = segment_points(s, clock);
        for (uint16_t p = 0; p < count && emitted < value_count; ++p, ++emitted) {
            jw_append(w, "%s", emitted ? "," : "");
            jw_time(w, segment_start + first + (double)p * period);
        }
        segment_start += (double)s->pulses * base;
    }
    jw_append(w, "]");
}

static const payload_v3_array_t *find_array(const payload_v3_trace_input_t *input,
                                             uint8_t index)
{
    for (size_t i = 0; i < input->array_count; ++i) {
        if (input->arrays[i].index == index) return &input->arrays[i];
    }
    return NULL;
}

static const char *series_name(uint8_t index)
{
    switch (index) {
    case 1: return "fluo_630_signal";
    case 2: return "fluo_630_ref";
    case 3: return "ambient_sun_vis";
    case 4: return "ambient_leaf_ir";
    case 5: return "refl_730_signal";
    case 6: return "refl_730_ref";
    default: return NULL;
    }
}

static series_clock_t series_clock(uint8_t index)
{
    if (index == 3U || index == 4U) return CLOCK_AMBIENT;
    if (index == 5U || index == 6U) return CLOCK_REFLECTION;
    return CLOCK_MAIN;
}

static bool duration_ms(const payload_v3_trace_input_t *input, uint32_t *value)
{
    const payload_v3_array_t *timing = find_array(input, 7U);
    if (timing == NULL || timing->values == NULL || timing->length < 2U) return false;
    const uint32_t delta_us = timing->values[1] - timing->values[0];
    *value = delta_us / 1000U;
    return true;
}

static void write_count_values(json_writer_t *w, const payload_v3_array_t *array)
{
    jw_append(w, "\"v\":[");
    for (uint16_t i = 0; i < array->length; ++i) {
        const uint32_t value = array->values[i] > 65535U ? 65535U : array->values[i];
        jw_append(w, "%s%u", i ? "," : "", (unsigned)value);
    }
    jw_append(w, "]");
}

static bool write_count_series(json_writer_t *w,
                               const payload_v3_trace_input_t *input,
                               const payload_v3_array_t *array,
                               bool *first)
{
    if (array == NULL || array->values == NULL || array->length == 0U) return true;
    char fallback[16];
    const char *name = series_name(array->index);
    if (name == NULL) {
        snprintf(fallback, sizeof fallback, "arr%u", (unsigned)array->index);
        name = fallback;
    }
    const series_clock_t clock = series_clock(array->index);
    const time_model_t model = analyze_time(input, clock, array->length);
    if (model.generated != array->length) return false;

    jw_append(w, "%s", *first ? "" : ",");
    *first = false;
    jw_string(w, name);
    jw_append(w, ":{\"u\":\"count\",");
    if (model.regular) {
        jw_append(w, "\"t0\":"); jw_time(w, model.t0);
        jw_append(w, ",\"dt\":"); jw_time(w, model.dt);
    } else {
        write_explicit_times(w, input, clock, array->length);
    }
    jw_append(w, ",");
    write_count_values(w, array);
    jw_append(w, "}");
    return w->ok;
}

static bool write_leaf_temp(json_writer_t *w,
                            const payload_v3_trace_input_t *input,
                            const payload_v3_array_t *env,
                            bool *first, bool duration_present,
                            uint32_t run_duration_ms)
{
    if (env == NULL || env->values == NULL || env->length == 0U) return true;
    const payload_v3_array_t *offsets = find_array(input, 8U);
    if (offsets != NULL && (offsets->values == NULL || offsets->length != env->length))
        return false;

    jw_append(w, "%s\"leaf_temp\":{\"u\":\"Cel\",\"t\":[",
              *first ? "" : ",");
    *first = false;
    if (offsets != NULL) {
        for (uint16_t i = 0; i < offsets->length; ++i) {
            jw_append(w, "%s", i ? "," : "");
            jw_time(w, (double)offsets->values[i] / 1000.0);
        }
    } else {
        const double freq = input->segments[0].freq;
        double delta = fmax(2.0, 8.0 / freq);
        const double duration_s = (double)run_duration_ms / 1000.0;
        /* A missing/zero timing trailer is not evidence that every sample was
         * acquired at t=0. Only a genuine positive duration may constrain the
         * cadence estimator. */
        if (duration_present && run_duration_ms > 0U && env->length > 1U &&
            (double)(env->length - 1U) * delta > duration_s)
            delta = duration_s / (double)(env->length - 1U);
        for (uint16_t i = 0; i < env->length; ++i) {
            jw_append(w, "%s", i ? "," : "");
            jw_time(w, (double)i * delta);
        }
    }
    jw_append(w, "]");
    if (offsets == NULL) jw_append(w, ",\"t_est\":true");
    jw_append(w, ",\"v\":[");
    for (uint16_t i = 0; i < env->length; ++i) {
        const int16_t centi = (int16_t)(env->values[i] & 0xFFFFU);
        jw_append(w, "%s%.2f", i ? "," : "", (double)centi / 100.0);
    }
    jw_append(w, "]}");
    return w->ok;
}

bool payload_v3_build_trace(char *out, size_t cap,
                            const payload_v3_trace_input_t *input,
                            char *error, size_t error_cap)
{
    json_writer_t w;
    jw_init(&w, out, cap);
    if (input == NULL || input->device == NULL || input->sensor_id == NULL ||
        input->channel == NULL || input->protocol_cmd == NULL ||
        input->segments == NULL || input->segment_count == 0U ||
        input->segment_count > PAYLOAD_V3_MAX_SEGMENTS ||
        input->arrays == NULL || input->array_count == 0U ||
        input->array_count > PAYLOAD_V3_MAX_ARRAYS ||
        !input->calibration_present || !isfinite(input->tick_factor) ||
        input->tick_factor <= 0.0 || input->tick_factor > PAYLOAD_V3_TICK_FACTOR_MAX) {
        set_error(error, error_cap, "invalid trace input");
        return false;
    }
    for (size_t i = 0; i < input->segment_count; ++i) {
        if (!valid_segment(&input->segments[i])) {
            set_error(error, error_cap, "invalid trace segment");
            return false;
        }
    }
    for (size_t i = 0; i < input->array_count; ++i) {
        if (input->arrays[i].length != 0U && input->arrays[i].values == NULL) {
            set_error(error, error_cap, "trace array data missing");
            return false;
        }
        for (size_t j = i + 1U; j < input->array_count; ++j) {
            if (input->arrays[i].index == input->arrays[j].index) {
                set_error(error, error_cap, "duplicate trace array index");
                return false;
            }
        }
    }
    const payload_v3_array_t *env = find_array(input, 0U);
    const payload_v3_array_t *offsets = find_array(input, 8U);
    if (offsets != NULL &&
        (env == NULL || offsets->values == NULL || offsets->length != env->length)) {
        set_error(error, error_cap, "leaf temperature offsets misaligned");
        return false;
    }
    const payload_v3_array_t *timing = find_array(input, 7U);
    if (timing == NULL || timing->values == NULL || timing->length != 2U) {
        set_error(error, error_cap, "calibration timing trailer missing or invalid");
        return false;
    }

    uint32_t run_duration_ms = 0;
    const bool duration_present = duration_ms(input, &run_duration_ms);
    jw_append(&w, "{\"schema\":\"ambit.trace/3\",\"measure_id\":%lld,\"channel\":",
              (long long)input->measure_id);
    jw_string(&w, input->channel);
    jw_append(&w, ",\"device\":"); jw_string(&w, input->device);
    jw_append(&w, ",\"sensor_id\":"); jw_string(&w, input->sensor_id);
    jw_append(&w, ",\"tag\":\"MEASUREMENT\",\"time\":{\"start_utc\":%lld,"
                  "\"end_utc\":%lld",
              (long long)input->start_utc_ms, (long long)input->end_utc_ms);
    if (duration_present)
        jw_append(&w, ",\"duration_ms\":%u", (unsigned)run_duration_ms);
    jw_append(&w, "},\"protocol\":{");
    bool comma = false;
    if (input->protocol_name != NULL && input->protocol_name[0] != '\0') {
        jw_append(&w, "\"name\":"); jw_string(&w, input->protocol_name); comma = true;
    }
    if (input->protocol_id != NULL && input->protocol_id[0] != '\0') {
        jw_append(&w, "%s\"id\":", comma ? "," : ""); jw_string(&w, input->protocol_id); comma = true;
    }
    jw_append(&w, "%s\"cmd\":", comma ? "," : ""); jw_string(&w, input->protocol_cmd);
    jw_append(&w, ",\"segments\":[");
    for (size_t i = 0; i < input->segment_count; ++i) {
        const payload_v3_segment_t *s = &input->segments[i];
        jw_append(&w, "%s{\"pulses\":%u,\"freq\":%u,\"actinic\":%u}",
                  i ? "," : "", (unsigned)s->pulses, (unsigned)s->freq,
                  (unsigned)s->actinic);
    }
    jw_append(&w, "],\"cal_version\":\"%08x\",\"tick_factor\":",
              (unsigned)input->cal_version);
    jw_time(&w, input->tick_factor);
    if (input->gains_present) {
        jw_append(&w, ",\"gains\":[%u,%u,%u,%u,%u,%u]",
                  input->gains[0], input->gains[1], input->gains[2],
                  input->gains[3], input->gains[4], input->gains[5]);
    }
    if (input->currents_present) {
        jw_append(&w, ",\"currents\":[%u,%u,%u]",
                  input->currents[0], input->currents[1], input->currents[2]);
    }
    jw_append(&w, "},\"series\":{");

    bool first = true;
    const uint8_t known_order[] = {1, 2, 3, 4, 5, 6};
    for (size_t i = 0; i < sizeof known_order; ++i) {
        if (!write_count_series(&w, input, find_array(input, known_order[i]), &first)) {
            set_error(error, error_cap, "series length does not match protocol");
            return false;
        }
    }
    for (size_t i = 0; i < input->array_count; ++i) {
        const uint8_t idx = input->arrays[i].index;
        if (idx <= 8U) continue;
        if (!write_count_series(&w, input, &input->arrays[i], &first)) {
            set_error(error, error_cap, "unknown series length does not match protocol");
            return false;
        }
    }
    if (!write_leaf_temp(&w, input, find_array(input, 0U), &first,
                         duration_present, run_duration_ms)) {
        set_error(error, error_cap, "leaf temperature offsets misaligned");
        return false;
    }
    jw_append(&w, "}}");
    if (!w.ok) {
        set_error(error, error_cap, "trace payload exceeds buffer");
        return false;
    }
    if (error != NULL && error_cap > 0) error[0] = '\0';
    return true;
}

static const char *legacy_array_tag(uint8_t index, char fallback[16])
{
    switch (index) {
    case 0: return "env";
    case 1: return "s_630";
    case 2: return "r_630";
    case 3: return "sun";
    case 4: return "leaf";
    case 5: return "s_730";
    case 6: return "r_730";
    case 7: return "timing";
    default:
        snprintf(fallback, 16, "arr%u", (unsigned)index);
        return fallback;
    }
}

static bool build_legacy_data(char *out, size_t cap,
                              const payload_v3_trace_input_t *input)
{
    json_writer_t w;
    jw_init(&w, out, cap);
    if (input == NULL || input->arrays == NULL || input->array_count == 0U ||
        input->array_count > PAYLOAD_V3_MAX_ARRAYS) return false;
    jw_append(&w, "{");
    for (size_t a = 0; a < input->array_count; ++a) {
        const payload_v3_array_t *array = &input->arrays[a];
        if (array->length != 0U && array->values == NULL) return false;
        char fallback[16];
        jw_append(&w, "%s", a ? "," : "");
        jw_string(&w, legacy_array_tag(array->index, fallback));
        jw_append(&w, ":[");
        for (uint16_t i = 0; i < array->length; ++i) {
            if (array->index == 0U) {
                const int16_t centi = (int16_t)(array->values[i] & 0xffffU);
                jw_append(&w, "%s%.2f", i ? "," : "", (double)centi / 100.0);
            } else {
                /* This is deliberately the frozen v2 representation: no
                 * clamp, rename, or omission, including additive arrays. */
                jw_append(&w, "%s%lu", i ? "," : "",
                          (unsigned long)array->values[i]);
            }
        }
        jw_append(&w, "]");
    }
    jw_append(&w, "}");
    return w.ok;
}

static bool build_legacy_metadata(char *out, size_t cap,
                                  const payload_v3_trace_input_t *input)
{
    json_writer_t w;
    jw_init(&w, out, cap);
    if (input == NULL || input->segments == NULL || input->segment_count == 0U ||
        input->segment_count > PAYLOAD_V3_MAX_SEGMENTS) return false;
    jw_append(&w, "{\"segments\":[");
    for (size_t i = 0; i < input->segment_count; ++i) {
        const payload_v3_segment_t *s = &input->segments[i];
        /* Frozen v2 metadata did not expose wire type/subsampling. */
        jw_append(&w, "%s{\"pulses\":%u,\"freq\":%u,\"actinic\":%u}",
                  i ? "," : "", (unsigned)s->pulses,
                  (unsigned)s->freq, (unsigned)s->actinic);
    }
    jw_append(&w, "],\"cal_version\":");
    if (input->calibration_present)
        jw_append(&w, "\"%08x\"", (unsigned)input->cal_version);
    else
        jw_append(&w, "null");
    if (input->gains_present) {
        jw_append(&w, ",\"gains\":[%u,%u,%u,%u,%u,%u]",
                  input->gains[0], input->gains[1], input->gains[2],
                  input->gains[3], input->gains[4], input->gains[5]);
    }
    if (input->currents_present) {
        jw_append(&w, ",\"currents\":[%u,%u,%u]",
                  input->currents[0], input->currents[1], input->currents[2]);
    }
    /* New-firmware v2 fallback is a permanent compatibility route. Preserve
     * the stable inventory join key whenever it was known; legacy consumers
     * already prefer this exact metadata spelling. */
    if (input->sensor_id != NULL && input->sensor_id[0] != '\0') {
        jw_append(&w, ",\"sensor_id\":");
        jw_string(&w, input->sensor_id);
    }
    if (input->protocol_name != NULL && input->protocol_name[0] != '\0') {
        jw_append(&w, ",\"protocol\":");
        jw_string(&w, input->protocol_name);
    }
    if (input->protocol_id != NULL && input->protocol_id[0] != '\0') {
        jw_append(&w, ",\"protocol_id\":");
        jw_string(&w, input->protocol_id);
    }
    jw_append(&w, "}");
    return w.ok;
}

static bool build_legacy_identity(char *out, size_t cap, const char *sensor_id)
{
    json_writer_t w;
    jw_init(&w, out, cap);
    if (sensor_id == NULL || sensor_id[0] == '\0') return false;
    jw_append(&w, "{\"sensor_id\":");
    jw_string(&w, sensor_id);
    jw_append(&w, "}");
    return w.ok;
}

payload_trace_route_t payload_v3_build_trace_lossless(
    char *out, size_t cap, char *metadata, size_t metadata_cap,
    const payload_v3_trace_input_t *input, char *error, size_t error_cap)
{
    if (metadata != NULL && metadata_cap != 0U) metadata[0] = '\0';
    char v3_error[96] = {0};
    if (payload_v3_build_trace(out, cap, input, v3_error, sizeof v3_error)) {
        if (error != NULL && error_cap != 0U) error[0] = '\0';
        return PAYLOAD_TRACE_ROUTE_V3;
    }

    if (!build_legacy_data(out, cap, input)) {
        set_error(error, error_cap, "v3 and v2 payload exceed buffer or arrays invalid");
        return PAYLOAD_TRACE_ROUTE_ERROR;
    }
    /* Keep the completed raw arrays even if verbose legacy metadata exceeds
     * its bounded region, but never report a lossless route after discarding a
     * known stable identity. The compact form preserves the platform's join
     * key; if even that cannot fit, fail explicitly instead of storing a v2 row
     * that cannot be associated with its sensor. */
    if (metadata != NULL && metadata_cap != 0U) {
        if (!build_legacy_metadata(metadata, metadata_cap, input) &&
            input != NULL && input->sensor_id != NULL && input->sensor_id[0] != '\0' &&
            !build_legacy_identity(metadata, metadata_cap, input->sensor_id)) {
            set_error(error, error_cap, "v2 fallback metadata cannot retain sensor identity");
            return PAYLOAD_TRACE_ROUTE_ERROR;
        }
    } else if (input != NULL && input->sensor_id != NULL && input->sensor_id[0] != '\0') {
        set_error(error, error_cap, "v2 fallback requires metadata for sensor identity");
        return PAYLOAD_TRACE_ROUTE_ERROR;
    }
    if (error != NULL && error_cap != 0U)
        snprintf(error, error_cap, "v2 fallback: %s", v3_error[0] ? v3_error : "not representable");
    return PAYLOAD_TRACE_ROUTE_V2;
}

static void write_attached(json_writer_t *w, const payload_v3_telemetry_input_t *input)
{
    jw_append(w, "\"attached_sensors\":[");
    bool first = true;
    for (size_t i = 0; i < input->attached_count && i < PAYLOAD_V3_MAX_ATTACHED; ++i) {
        const payload_v3_attached_sensor_t *a = &input->attached[i];
        if (!a->present || a->channel == NULL || a->sensor_id == NULL) continue;
        jw_append(w, "%s{\"channel\":", first ? "" : ","); first = false;
        jw_string(w, a->channel); jw_append(w, ",\"sensor_id\":"); jw_string(w, a->sensor_id);
        if (a->firmware != NULL && a->firmware[0]) { jw_append(w, ",\"firmware\":"); jw_string(w, a->firmware); }
        if (a->hardware_revision != 0U) jw_append(w, ",\"hardware_revision\":%u", a->hardware_revision);
        if (a->name != NULL && a->name[0]) { jw_append(w, ",\"name\":"); jw_string(w, a->name); }
        if (a->cal_version_present)
            jw_append(w, ",\"cal_version\":\"%08x\"", (unsigned)a->cal_version);
        jw_append(w, "}");
    }
    jw_append(w, "]");
}

bool payload_v3_build_telemetry(char *out, size_t cap,
                                const payload_v3_telemetry_input_t *input,
                                char *error, size_t error_cap)
{
    json_writer_t w;
    jw_init(&w, out, cap);
    if (input == NULL || input->device == NULL || input->attached_count > PAYLOAD_V3_MAX_ATTACHED ||
        (input->observations_valid &&
         (!isfinite(input->air_temperature) || !isfinite(input->relative_humidity) ||
          !isfinite(input->air_pressure))) ||
        (input->power_valid &&
         (!isfinite(input->battery_v) || !isfinite(input->input_v) ||
          !isfinite(input->system_v)))) {
        set_error(error, error_cap, "invalid telemetry input");
        return false;
    }
    jw_append(&w, "{\"schema\":\"ambyte.telemetry/1\",\"measure_id\":%lld,\"device\":",
              (long long)input->measure_id);
    jw_string(&w, input->device);
    jw_append(&w, ",\"tag\":\"TELEMETRY\",\"time\":{\"observed_utc\":%lld},\"observations\":{",
              (long long)input->observed_utc_ms);
    if (input->observations_valid) {
        jw_append(&w, "\"air_temperature\":{\"u\":\"Cel\",\"v\":%.2f},"
                      "\"relative_humidity\":{\"u\":\"%%RH\",\"v\":%.2f},"
                      "\"air_pressure\":{\"u\":\"Pa\",\"v\":%.1f}",
                  input->air_temperature, input->relative_humidity, input->air_pressure);
    }
    jw_append(&w, "},\"health\":{\"connectivity\":{");
    if (input->connectivity_valid) {
        jw_append(&w, "\"wifi\":%s,\"provisioned\":%s,\"publish_gate\":%s,"
                      "\"mqtt_reconnects\":%u,\"last_disc_reason\":",
                  input->wifi ? "true" : "false", input->provisioned ? "true" : "false",
                  input->publish_gate ? "true" : "false", input->mqtt_reconnects);
        jw_string(&w, input->last_disc_reason);
        jw_append(&w, ",\"conn_age_s\":%lld,\"pending\":%lld",
                  (long long)input->conn_age_s, (long long)input->pending);
    }
    jw_append(&w, "},\"power\":{");
    if (input->power_valid) {
        jw_append(&w, "\"battery_v\":%.3f,\"input_v\":%.3f,\"system_v\":%.3f,"
                      "\"input_ma\":%u,\"charge_ma\":%u,\"input_present\":%s,"
                      "\"charge_status\":%u",
                  input->battery_v, input->input_v, input->system_v,
                  input->input_ma, input->charge_ma,
                  input->input_present ? "true" : "false", input->charge_status);
    }
    jw_append(&w, "},\"storage\":{");
    bool storage_comma = false;
    if (input->storage_db_valid) {
        jw_append(&w, "\"db_online\":%s", input->db_online ? "true" : "false");
        storage_comma = true;
    }
    if (input->storage_sd_valid) {
        jw_append(&w, "%s\"sd_free_kb\":%llu,\"sd_skipped\":%lld,"
                      "\"sd_dropped\":%lld,\"last_acked_id\":%lld,\"sd_io_lost\":%s",
                  storage_comma ? "," : "",
                  (unsigned long long)input->sd_free_kb,
                  (long long)input->sd_skipped, (long long)input->sd_dropped,
                  (long long)input->last_acked_id,
                  input->sd_io_lost ? "true" : "false");
    }
    jw_append(&w, "},\"runtime\":{");
    if (input->runtime_valid) {
        jw_append(&w, "\"uptime_s\":%lld,\"psram_free_kb\":%u,\"psram_largest_kb\":%u,"
                      "\"psram_size_kb\":%u,\"heap_dma_largest_kb\":%u,"
                      "\"heap_int_free_kb\":%u,\"heap_int_largest_kb\":%u,"
                      "\"wd_armed\":%s,\"last_wd_reboot_reason\":",
                  (long long)input->uptime_s, input->psram_free_kb,
                  input->psram_largest_kb, input->psram_size_kb,
                  input->heap_dma_largest_kb, input->heap_int_free_kb,
                  input->heap_int_largest_kb, input->wd_armed ? "true" : "false");
        jw_string(&w, input->last_wd_reboot_reason);
    }
    jw_append(&w, "},\"clock\":{");
    if (input->clock_valid) {
        jw_append(&w, "\"source\":"); jw_string(&w, input->clock_source);
        jw_append(&w, ",\"suspect\":%s", input->clock_suspect ? "true" : "false");
    }
    jw_append(&w, "},\"software\":{");
    if (input->software_valid) {
        jw_append(&w, "\"firmware\":"); jw_string(&w, input->firmware);
        if (input->script_valid) {
            jw_append(&w, ",\"script_sha256\":"); jw_string(&w, input->script_sha256);
            jw_append(&w, ",\"script_version\":"); jw_string(&w, input->script_version);
            jw_append(&w, ",\"script_built_against_fw\":"); jw_string(&w, input->script_built_against_fw);
            jw_append(&w, ",\"script_installed_on_fw\":"); jw_string(&w, input->script_installed_on_fw);
            jw_append(&w, ",\"script_metadata_verified\":%s",
                      input->script_metadata_verified ? "true" : "false");
        }
    }
    jw_append(&w, "},");
    write_attached(&w, input);
    jw_append(&w, "}}");
    if (!w.ok) {
        set_error(error, error_cap, "telemetry payload exceeds buffer");
        return false;
    }
    if (error != NULL && error_cap > 0) error[0] = '\0';
    return true;
}

bool payload_v3_build_device(char *out, size_t cap,
                             const payload_v3_device_input_t *input,
                             char *error, size_t error_cap)
{
    json_writer_t w;
    jw_init(&w, out, cap);
    if (input == NULL || input->channel == NULL || input->device == NULL ||
        input->sensor_id == NULL || input->name == NULL || input->firmware == NULL ||
        !isfinite(input->temp_offset) || !isfinite(input->temp_slope) ||
        !isfinite(input->actinic_coef) || !isfinite(input->spec_coef) ||
        !isfinite(input->mlx_emissivity) || !isfinite(input->sun_coef) ||
        !isfinite(input->tick_factor) || input->tick_factor <= 0.0 ||
        input->tick_factor > PAYLOAD_V3_TICK_FACTOR_MAX) {
        set_error(error, error_cap, "invalid device input");
        return false;
    }
    jw_append(&w, "{\"schema\":\"ambit.device/1\",\"measure_id\":%lld,\"channel\":",
              (long long)input->measure_id);
    jw_string(&w, input->channel); jw_append(&w, ",\"device\":"); jw_string(&w, input->device);
    jw_append(&w, ",\"tag\":\"DEVICE_INFO\",\"time\":{\"observed_utc\":%lld},"
                  "\"identity\":{\"sensor_id\":", (long long)input->observed_utc_ms);
    jw_string(&w, input->sensor_id); jw_append(&w, ",\"name\":"); jw_string(&w, input->name);
    jw_append(&w, ",\"firmware\":"); jw_string(&w, input->firmware);
    if (input->hardware_revision != 0U)
        jw_append(&w, ",\"hardware_revision\":%u", input->hardware_revision);
    jw_append(&w, ",\"cal_version\":\"%08x\"},\"calibration\":{\"mlx_coef\":[",
              (unsigned)input->cal_version);
    for (size_t i = 0; i < 14U; ++i)
        jw_append(&w, "%s%ld", i ? "," : "", (long)input->mlx_coef[i]);
    jw_append(&w, "],\"adpd\":[");
    for (size_t i = 0; i < 6U; ++i)
        jw_append(&w, "%s%lu", i ? "," : "", (unsigned long)input->adpd[i]);
    jw_append(&w, "],\"temp_offset\":%.4f,\"temp_slope\":%.4f,"
                  "\"actinic_coef\":%.6f,\"spec_coef\":%.6f,"
                  "\"act\":[%u,%u,%u,%u,%u],\"mlx_emissivity\":%.4f,"
                  "\"sun_coef\":%.6f,\"tick_factor\":%.6f}}",
              input->temp_offset, input->temp_slope, input->actinic_coef,
              input->spec_coef, input->act[0], input->act[1], input->act[2],
              input->act[3], input->act[4], input->mlx_emissivity,
              input->sun_coef, input->tick_factor);
    if (!w.ok) {
        set_error(error, error_cap, "device payload exceeds buffer");
        return false;
    }
    if (error != NULL && error_cap > 0) error[0] = '\0';
    return true;
}

bool payload_v3_is_canonical_object(const char *json)
{
    if (json == NULL) return false;
    /* Event publishing routes without reparsing JSON, so every canonical
     * builder must keep schema as the first member and this exact prefix list
     * must change atomically with any new firmware-owned family. */
#define SCHEMA_PREFIX(s) strncmp(json, (s), sizeof(s) - 1U) == 0
    return SCHEMA_PREFIX("{\"schema\":\"ambit.trace/3\"") ||
           SCHEMA_PREFIX("{\"schema\":\"ambyte.telemetry/1\"") ||
           SCHEMA_PREFIX("{\"schema\":\"ambit.device/1\"");
#undef SCHEMA_PREFIX
}

bool payload_v3_can_fetch_retained(bool trigger_valid, size_t segment_count)
{
    return trigger_valid && segment_count > 0U &&
           segment_count <= PAYLOAD_V3_MAX_SEGMENTS;
}
