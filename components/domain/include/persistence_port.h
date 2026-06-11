#ifndef AMBYTE_PERSISTENCE_PORT_H
#define AMBYTE_PERSISTENCE_PORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MEASUREMENT_SYNC_PENDING  = 0,
    MEASUREMENT_SYNC_INFLIGHT = 1,
    MEASUREMENT_SYNC_SYNCED   = 2,
} measurement_sync_state_t;

/* Origin tags (schema v2). Firmware-assigned — never user strings. */
#define MEASUREMENT_TAG_MEASUREMENT "MEASUREMENT"   /* script-originated data   */
#define MEASUREMENT_TAG_STATUS      "STATUS"        /* background status report */

/* One row per measurement *event*. All quantities measured together (a BME280
 * read's T/H/P, or an AMBIT run's leaf_temp/fluor/fluoRef arrays) live in one
 * row, serialised as a JSON object in `payload_json`. This collapses the old
 * row-per-quantity design into a single store-and-forward document.
 *
 * Schema v2 provenance (firmware-filled; "" publishes as JSON null):
 * channel:       physical port, "uart_<n>" / "usb_<n>"; "" = onboard.
 * device:        discovered sensor name ("ambit"); "" = unknown/onboard.
 * tag:           origin class, one of MEASUREMENT_TAG_*.
 * cmd_raw:       the command that produced the data, in the target device's own
 *                vocabulary ("arrun", "get_par") or the literal text command.
 * metadata_json: JSON object string, or NULL.
 * payload_json:  JSON object of quantities, e.g. {"temperature":23.1,...} or
 *                {"s_fluo":[...],"r_fluo":[...]}. Heap on claim; caller frees
 *                via measurement_event_free.
 */
typedef struct {
    int64_t  measure_id;
    char     channel[12];
    char     device[24];
    char     tag[16];
    char     cmd_raw[40];
    int64_t  start_ticks_ms;
    int64_t  end_ticks_ms;
    char    *metadata_json;
    char    *payload_json;
    measurement_sync_state_t sync_state;
} measurement_event_t;

/* Store descriptor: what a producer hands to the store. All strings are
 * caller-owned and copied by the store; NULL and "" are equivalent (absent)
 * except payload_json and tag, which are required. */
typedef struct {
    int64_t     measure_id;
    const char *channel;
    const char *device;
    const char *tag;
    const char *cmd_raw;
    int64_t     start_ms;
    int64_t     end_ms;
    const char *metadata_json;
    const char *payload_json;
} measurement_event_desc_t;

static inline void measurement_event_free(measurement_event_t *e)
{
    if (e == NULL) return;
    free(e->metadata_json);
    free(e->payload_json);
    e->metadata_json = NULL;
    e->payload_json  = NULL;
}

/* ── port function-pointer typedefs ──────────────────────────────────── */

/* Allocate the next monotonic measure_id. */
typedef esp_err_t (*measurement_next_id_fn)(int64_t *out_id);

/* Store one event from a descriptor. desc->payload_json and desc->tag are
 * required; the other strings may be NULL/"". Requires the store to be
 * available (SD mounted). */
typedef esp_err_t (*measurement_store_event_fn)(const measurement_event_desc_t *desc);

/* Claim the oldest PENDING event, flip it INFLIGHT, return it (heap strings —
 * free with measurement_event_free). ESP_ERR_NOT_FOUND when none pending. */
typedef esp_err_t (*measurement_claim_next_event_fn)(measurement_event_t *out);

typedef esp_err_t (*measurement_mark_event_synced_fn)(int64_t measure_id);
typedef esp_err_t (*measurement_mark_event_pending_fn)(int64_t measure_id);

/* Read-only event-table stats (no mutation). *available = DB open (SD mounted);
 * *total = row count; *pending = rows not yet synced (sync_state != SYNCED);
 * *next_id = the next measure_id to be allocated. Any out-pointer may be NULL.
 * Returns ESP_OK even when the DB is offline (*available=false, counts 0). */
typedef esp_err_t (*measurement_db_stats_fn)(bool *available, int64_t *total,
                                             int64_t *pending, int64_t *next_id);

#ifdef __cplusplus
}
#endif

#endif
