#ifndef AMBYTE_PAYLOAD_SCALAR_H
#define AMBYTE_PAYLOAD_SCALAR_H

/*
 * payload_scalar — one bounded string-value lookup in stored payload JSON.
 *
 * Exists for per-row macro routing (envelope_provenance.c): the publisher
 * must evaluate a macro's `when:` conditions against the row it is about to
 * publish, on the sync_runner task, with no heap and no cJSON (pulling a full
 * JSON parser onto the publish path for a handful of scalar reads is not
 * worth its code size or its allocations). The scanner matches STRUCTURE, not
 * position: the canonical v3 builders emit these keys at fixed offsets, but
 * stored v2 rows have a different shape and must scan correctly too.
 *
 * key_path is either a bare top-level key ("schema", "tag", "channel",
 * "device", "sensor_id") or a one-level dotted path ("protocol.name",
 * "protocol.tag") — for the dotted form the parent object is located first
 * and the key is searched only within that object's bounds (the top level
 * also has a "tag" key, which must NOT answer a "protocol.tag" query).
 * Matching is depth-exact: only keys at the immediate level of the searched
 * object count, so a telemetry row's nested attached_sensors[].channel never
 * answers a top-level "channel" query.
 *
 * Addressability for non-canonical (v2) payloads: only tag/channel/device at
 * the v2 top level are meaningful; `schema` is ABSENT there (absence is a
 * legitimate, matchable condition outcome — see envelope_provenance.h).
 *
 * Values with JSON escapes are not expected for any of these fields (builders
 * emit plain ASCII ids and tags); \" and \\ are collapsed minimally during
 * the copy so a stray escape cannot wedge the scan, anything more exotic is
 * copied as-is after the backslash is dropped.
 *
 * Returns true and a NUL-terminated value in out when the key is found with a
 * string value that fits; false on a missing key, a non-string value,
 * truncated/malformed JSON, or a value that does not fit cap (a value ≥ cap
 * can never equal a compiler-capped condition value, so callers may treat it
 * as absent — see envelope_provenance.c).
 */

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

bool payload_scalar(const char *json, const char *key_path, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* AMBYTE_PAYLOAD_SCALAR_H */
