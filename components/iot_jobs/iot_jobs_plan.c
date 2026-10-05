#include "iot_jobs_plan.h"

#include <stddef.h>
#include <string.h>

#define SHA256_HEX_LEN 64

static const char *strip_v(const char *s)
{
    return (s[0] == 'v' || s[0] == 'V') ? s + 1 : s;
}

bool iot_jobs_versions_equal(const char *a, const char *b)
{
    if (a == NULL || b == NULL) return false;
    a = strip_v(a);
    b = strip_v(b);
    return a[0] != '\0' && strcmp(a, b) == 0;
}

static bool is_sha256_hex(const char *s)
{
    if (s == NULL || strlen(s) != SHA256_HEX_LEN) return false;
    for (const char *p = s; *p != '\0'; p++) {
        char c = *p;
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
            return false;
        }
    }
    return true;
}

iot_jobs_action_t iot_jobs_plan(const iot_jobs_document_t *doc,
                                const iot_jobs_device_t *dev,
                                const char **reason)
{
    const char *unused;
    if (reason == NULL) reason = &unused;

    /* REJECTED, not FAILED: the rollout's abort criteria count FAILED, and a
     * document this firmware cannot act on says nothing about the image's
     * health. The workflow still flags REJECTED executions as not succeeded. */
    if (doc->operation == NULL || strcmp(doc->operation, "firmware-update") != 0) {
        *reason = "unsupported operation";
        return IOT_JOBS_REJECT;
    }
    /* The contract has no device-side family check upstream; the workflow
     * narrows targets by the registry's deviceType, and this is the backstop
     * against flashing another family's image, whose digest would match. */
    if (doc->family == NULL || strcmp(doc->family, dev->family) != 0) {
        *reason = "job is for another device family";
        return IOT_JOBS_REJECT;
    }
    if (doc->version == NULL || doc->version[0] == '\0') {
        *reason = "job document has no version";
        return IOT_JOBS_REJECT;
    }

    /* Before the version comparison: right after an update the new version is
     * already running but can still roll back, so a match is not yet success.
     * The confirm path re-queries once the image is marked valid. */
    if (dev->pending_verify) {
        *reason = "waiting for the running image to be confirmed";
        return IOT_JOBS_DEFER;
    }
    if (iot_jobs_versions_equal(doc->version, dev->running_version)) {
        *reason = "running this version";
        return IOT_JOBS_SUCCEED;
    }
    /* This job's image was written and booted, yet the running version is not
     * the job's: the health gate or the bootloader rolled it back. Installing
     * again would loop the same bad image until the job times out. */
    if (dev->applied_this_job) {
        *reason = "new image rolled back after reboot";
        return IOT_JOBS_FAIL;
    }
    if (!is_sha256_hex(doc->sha256)) {
        *reason = "job document has no valid sha256";
        return IOT_JOBS_REJECT;
    }
    if (doc->url == NULL || strncmp(doc->url, "https://", 8) != 0) {
        *reason = "job document has no https url";
        return IOT_JOBS_REJECT;
    }
    *reason = "installing";
    return IOT_JOBS_INSTALL;
}
