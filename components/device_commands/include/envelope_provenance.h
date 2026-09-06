#ifndef AMBYTE_ENVELOPE_PROVENANCE_H
#define AMBYTE_ENVELOPE_PROVENANCE_H

/*
 * envelope_provenance — the publish envelope's workbook-provenance splice,
 * extracted as a pure function so it is host-testable (tests/
 * envelope_provenance_host.c). No ESP-IDF dependency: the inputs are the
 * domain port struct and the published row's payload JSON, scanned with
 * payload_codec's payload_scalar for the per-macro `when:` routing.
 *
 * The three DC_*_ENVELOPE_FMT macros also live here — they are the exact
 * format strings device_commands.c publishes with, and keeping them in a
 * header lets the host test render the REAL formats (a malformed key in the
 * C source would otherwise pass every Python-side reconstruction test).
 */

#include <stddef.h>

#include "schedule_provenance_port.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DC_EVENT_ENVELOPE_FMT \
        "{\"sample\":[{" \
            "\"v\":2,\"measure_id\":%lld,\"startTicks_UTC\":%lld,\"endTicks_UTC\":%lld," \
            "\"timestamp_local\":\"%s\"," \
            "\"published\":\"%s\",\"channel\":%s,\"device\":%s," \
            "\"cmd_raw\":%s,\"tag\":\"%s\",\"metadata\":%s,\"data\":%s" \
        "}]," \
        "\"timestamp\":\"%s\",%s%s%s" \
        "\"device_id\":\"%s\",\"device_name\":\"%s\"," \
        "\"device_version\":\"%s\",\"device_firmware\":\"%s\"}"

#define DC_V3_EVENT_ENVELOPE_FMT \
        "{\"sample\":[%s],\"timestamp\":\"%s\",%s%s%s" \
        "\"device_id\":\"%s\",\"device_name\":\"%s\"," \
        "\"device_version\":\"%s\",\"device_firmware\":\"%s\"}"

/* Gzip transport variant of the v3 envelope, emitted only while the runtime
 * publish_gzip switch is on AND the compressed form is strictly smaller.
 * `sample` becomes base64(gzip("[<canonical v3 object>]")) — the exact JSON
 * text the plain envelope would splice — and `_sample_encoding` is the marker
 * the OpenJII Silver layer already reverses for the mobile uploader
 * (decompress_sample_value). The outer envelope stays plain JSON so broker
 * routing and raw storage are unaffected. */
#define DC_V3_GZ_EVENT_ENVELOPE_FMT \
        "{\"sample\":\"%s\",\"_sample_encoding\":\"gzip+base64\"," \
        "\"timestamp\":\"%s\",%s%s%s" \
        "\"device_id\":\"%s\",\"device_name\":\"%s\"," \
        "\"device_version\":\"%s\",\"device_firmware\":\"%s\"}"

/* Render the optional provenance part (the third "%s" splice beside
 * battpart/tzpart in every format above), filtered per published row:
 *   "workbook_version_id":"<id>",   — only when prov->workbook_version_id is set
 *   "macros":[{"id":..,"name":..,"filename":..},…], — only when at least one
 *                                     macro applies to this row
 * payload_json is the stored payload text of the event being published (the
 * sample object, not the envelope); each macro's `when:` conditions are
 * evaluated AND-ed against it via payload_scalar. A field ABSENT from the
 * payload makes eq false and neq true — the asymmetry that lets a "legacy/v2"
 * macro be written as schema neq each of the four canonical v3 schemas.
 * Returns the number of characters written (excluding NUL); 0 means the part
 * is empty and the envelope stays byte-identical to a schedule without a
 * stamped header. An empty result is also returned when cap cannot hold the
 * FULL part: a truncated splice would be malformed JSON, and a missing key
 * beats a broken envelope (device buffers are sized per the documented
 * worst case, so this is purely defensive).
 *
 * The strings are spliced WITHOUT JSON escaping. That is sound because the
 * schedule compiler rejects any macro id/name/filename character outside
 * [A-Za-z0-9_.:-] (sched_compile.c); workbook_version_id predates that rule,
 * so every string is re-checked here and an unsafe value drops its key
 * rather than corrupting the envelope JSON. The macro list was all-or-nothing
 * before `when:` existed, because a partially published list would have made
 * the gold pipeline process a row with the wrong macro set; per-row condition
 * filtering is now exactly what the pipeline wants — openJII runs, per row,
 * only the macros listed in that row's envelope. All-or-nothing survives only
 * among the macros that DO render: one unsafe rendered string still drops the
 * whole macros key. */
int envelope_provenance_part(const schedule_provenance_t *prov,
                             const char *payload_json, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* AMBYTE_ENVELOPE_PROVENANCE_H */
