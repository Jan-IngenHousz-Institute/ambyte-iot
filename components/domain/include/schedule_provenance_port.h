#ifndef AMBYTE_SCHEDULE_PROVENANCE_PORT_H
#define AMBYTE_SCHEDULE_PROVENANCE_PORT_H

/*
 * schedule_provenance_port — schedule-header provenance for the MQTT publish
 * envelope ("workbook_version_id" + "macros" keys).
 *
 * The envelope builder (device_commands) needs the compiled schedule's
 * workbook provenance once per publish, but sched_runner already depends on
 * device_commands — a direct call would close a component cycle. Like the
 * other ports in this directory, the dependency is inverted: app_main wires
 * sched_runner_provenance_port() into the device_commands config and this
 * header is the only shared vocabulary.
 *
 * Field widths mirror sched_header_t's macro snapshot, whose widths mirror
 * the schedule compiler's caps: id is a 36-char uuid (8-4-4-4-12), name and
 * filename are capped at 47 chars, and every character is restricted to
 * [A-Za-z0-9_.:-] at compile time — that restriction is what lets the
 * envelope splice these strings without a JSON-escaping pass.
 */

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SCHEDULE_PROVENANCE_MAX_MACROS 8 /* == SCHED_SPEC_MAX_MACROS (asserted
                                          * in sched_runner.c) */
#define SCHEDULE_PROVENANCE_MAX_MACRO_CONDS 4 /* == SCHED_SPEC_MAX_MACRO_CONDS
                                          * (asserted in sched_runner.c) */

/* Mirror of sched_spec.h's sched_cond_field_t / sched_cond_op_t — this header
 * is the only shared vocabulary between sched_runner and device_commands
 * (dependency inversion: device_commands must NOT include sched_spec.h), so
 * the condition model is restated here and sched_runner.c compile-asserts
 * that the values match, member for member. */
typedef enum {
    SCHED_PROV_COND_FIELD_SCHEMA,
    SCHED_PROV_COND_FIELD_TAG,
    SCHED_PROV_COND_FIELD_CHANNEL,
    SCHED_PROV_COND_FIELD_DEVICE,
    SCHED_PROV_COND_FIELD_SENSOR_ID,
    SCHED_PROV_COND_FIELD_PROTOCOL_NAME,
    SCHED_PROV_COND_FIELD_PROTOCOL_TAG,
} sched_prov_cond_field_t;

typedef enum {
    SCHED_PROV_COND_OP_EQ,
    SCHED_PROV_COND_OP_NEQ,
} sched_prov_cond_op_t;

typedef struct {
    char id[40];
    char name[48];
    char filename[48];
    /* Compiled `when:` conditions, AND-ed per published row. Strings are
     * materialized (not pool offsets) because this struct crosses the
     * component boundary by value. cond_count == 0 = the macro applies to
     * every row. value chars are compiler-restricted to [A-Za-z0-9_.:/-]
     * — one wider than the id/name/filename alphabet, because schema ids
     * like "ambit.trace/3" contain '/'. */
    uint8_t cond_count;
    struct {
        uint8_t field;      /* sched_prov_cond_field_t */
        uint8_t op;         /* sched_prov_cond_op_t */
        char    value[48];  /* ≤ 47 chars + NUL (compiler cap) */
    } conds[SCHEDULE_PROVENANCE_MAX_MACRO_CONDS];
} schedule_provenance_macro_t;

typedef struct {
    char    workbook_version_id[64];     /* "" = header declares none → omit the key */
    uint8_t macro_count;                 /* 0 = omit the "macros" key */
    schedule_provenance_macro_t macros[SCHEDULE_PROVENANCE_MAX_MACROS];
} schedule_provenance_t;

/* ESP_OK + a filled snapshot; any error (e.g. no schedule started yet) means
 * "no provenance" and the envelope omits both keys. Called once per publish
 * on the sync_runner task, so it must be cheap — a bounded struct copy. */
typedef esp_err_t (*schedule_provenance_fn)(schedule_provenance_t *out);

#ifdef __cplusplus
}
#endif

#endif /* AMBYTE_SCHEDULE_PROVENANCE_PORT_H */
