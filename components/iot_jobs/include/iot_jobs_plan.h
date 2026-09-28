#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * What to do with one AWS IoT job execution, as a pure function of the job
 * document and the device's own state. Kept free of ESP-IDF so the whole
 * contract is host-testable (test/iot_jobs).
 *
 * The contract is openJII's (apps/docs/content/developers/device-integration.mdx,
 * "Firmware updates"), produced by its firmware-rollout workflow:
 *   { "operation": "firmware-update", "family": "ambyte", "version": "v1.3.0",
 *     "sha256": "<64 hex>", "url": "<presigned, expires within the hour>" }
 */

typedef enum {
    IOT_JOBS_REJECT,   /* not a job this device can run: REJECTED */
    IOT_JOBS_DEFER,    /* an unconfirmed image is booted; decide after confirm */
    IOT_JOBS_SUCCEED,  /* already running this version: SUCCEEDED, no flash */
    IOT_JOBS_FAIL,     /* this job's image was applied and rolled back: FAILED */
    IOT_JOBS_INSTALL,  /* download, verify, flash, reboot */
} iot_jobs_action_t;

typedef struct {
    const char *operation;
    const char *family;
    const char *version;
    const char *sha256;
    const char *url;
} iot_jobs_document_t;   /* NULL = field absent or not a string */

typedef struct {
    const char *family;            /* this firmware's family, "ambyte" */
    const char *running_version;   /* compiled app version */
    bool        pending_verify;    /* running image not yet confirmed valid */
    bool        applied_this_job;  /* ota latch holds this job's id */
} iot_jobs_device_t;

/* `*reason` gets a short static explanation for the status report. */
iot_jobs_action_t iot_jobs_plan(const iot_jobs_document_t *doc,
                                const iot_jobs_device_t *dev,
                                const char **reason);

/* Release tags read `v1.3.0`, builds stamp `1.3.0`: equal ignoring one leading
 * v on either side, otherwise exact (a `1.3.0-2-gabc123` build is not 1.3.0). */
bool iot_jobs_versions_equal(const char *a, const char *b);

#ifdef __cplusplus
}
#endif
