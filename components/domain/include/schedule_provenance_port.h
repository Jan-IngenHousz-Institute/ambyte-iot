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

typedef struct {
    char    workbook_version_id[64];     /* "" = header declares none → omit the key */
    uint8_t macro_count;                 /* 0 = omit the "macros" key */
    struct {
        char id[40];
        char name[48];
        char filename[48];
    } macros[SCHEDULE_PROVENANCE_MAX_MACROS];
} schedule_provenance_t;

/* ESP_OK + a filled snapshot; any error (e.g. no schedule started yet) means
 * "no provenance" and the envelope omits both keys. Called once per publish
 * on the sync_runner task, so it must be cheap — a bounded struct copy. */
typedef esp_err_t (*schedule_provenance_fn)(schedule_provenance_t *out);

#ifdef __cplusplus
}
#endif

#endif /* AMBYTE_SCHEDULE_PROVENANCE_PORT_H */
