#include <stdio.h>
#include <string.h>

#include "iot_jobs_plan.h"

#define SHA "0123456789abcdef0123456789abcdef0123456789abcdef0123456789ABCDEF"
#define URL "https://s3.eu-central-1.amazonaws.com/open-jii-firmware-dev/ambyte/v1.3.0/x/firmware.bin?X-Amz-Signature=abc"

static unsigned s_failures = 0;

#define CHECK(expression)                                                        \
    do {                                                                         \
        if (!(expression)) {                                                     \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n",                       \
                    __FILE__, __LINE__, #expression);                            \
            s_failures++;                                                        \
        }                                                                        \
    } while (0)

static iot_jobs_document_t good_doc(void)
{
    return (iot_jobs_document_t) {
        .operation = "firmware-update",
        .family    = "ambyte",
        .version   = "v1.3.0",
        .sha256    = SHA,
        .url       = URL,
    };
}

static iot_jobs_device_t device_on(const char *version)
{
    return (iot_jobs_device_t) {
        .family           = "ambyte",
        .running_version  = version,
        .pending_verify   = false,
        .applied_this_job = false,
    };
}

static iot_jobs_action_t plan(const iot_jobs_document_t *doc, const iot_jobs_device_t *dev)
{
    const char *reason = NULL;
    iot_jobs_action_t action = iot_jobs_plan(doc, dev, &reason);
    CHECK(reason != NULL && reason[0] != '\0');
    return action;
}

static void test_versions_equal(void)
{
    CHECK(iot_jobs_versions_equal("v1.3.0", "1.3.0"));
    CHECK(iot_jobs_versions_equal("1.3.0", "v1.3.0"));
    CHECK(iot_jobs_versions_equal("V1.3.0", "1.3.0"));
    CHECK(iot_jobs_versions_equal("1.0.6-rc1", "v1.0.6-rc1"));
    CHECK(!iot_jobs_versions_equal("v1.3.0", "1.3.0-2-gabc123"));
    CHECK(!iot_jobs_versions_equal("v1.3.0", "1.3"));
    CHECK(!iot_jobs_versions_equal("vv1.3.0", "1.3.0"));
    CHECK(!iot_jobs_versions_equal("v", ""));
    CHECK(!iot_jobs_versions_equal(NULL, "1.3.0"));
}

static void test_install_when_behind(void)
{
    iot_jobs_document_t doc = good_doc();
    iot_jobs_device_t dev = device_on("1.2.0");
    CHECK(plan(&doc, &dev) == IOT_JOBS_INSTALL);
}

static void test_succeed_without_flash_when_current(void)
{
    iot_jobs_document_t doc = good_doc();
    iot_jobs_device_t dev = device_on("1.3.0");
    CHECK(plan(&doc, &dev) == IOT_JOBS_SUCCEED);

    /* The post-reboot report of our own update is the same case. */
    dev.applied_this_job = true;
    CHECK(plan(&doc, &dev) == IOT_JOBS_SUCCEED);
}

static void test_defer_while_image_unconfirmed(void)
{
    iot_jobs_document_t doc = good_doc();
    iot_jobs_device_t dev = device_on("1.3.0");
    dev.pending_verify = true;
    dev.applied_this_job = true;
    CHECK(plan(&doc, &dev) == IOT_JOBS_DEFER);

    /* Also for a different job: no second update over an unconfirmed image. */
    dev = device_on("1.3.0");
    dev.pending_verify = true;
    doc.version = "v1.4.0";
    CHECK(plan(&doc, &dev) == IOT_JOBS_DEFER);
}

static void test_fail_after_rollback(void)
{
    iot_jobs_document_t doc = good_doc();
    iot_jobs_device_t dev = device_on("1.2.0");
    dev.applied_this_job = true;
    CHECK(plan(&doc, &dev) == IOT_JOBS_FAIL);
}

static void test_reject_unusable_documents(void)
{
    iot_jobs_device_t dev = device_on("1.2.0");
    iot_jobs_document_t doc;

    doc = good_doc(); doc.operation = NULL;
    CHECK(plan(&doc, &dev) == IOT_JOBS_REJECT);
    doc = good_doc(); doc.operation = "reboot";
    CHECK(plan(&doc, &dev) == IOT_JOBS_REJECT);
    doc = good_doc(); doc.family = "ambit";
    CHECK(plan(&doc, &dev) == IOT_JOBS_REJECT);
    doc = good_doc(); doc.family = NULL;
    CHECK(plan(&doc, &dev) == IOT_JOBS_REJECT);
    doc = good_doc(); doc.version = "";
    CHECK(plan(&doc, &dev) == IOT_JOBS_REJECT);
    doc = good_doc(); doc.sha256 = "abc";
    CHECK(plan(&doc, &dev) == IOT_JOBS_REJECT);
    doc = good_doc(); doc.sha256 = "g123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    CHECK(plan(&doc, &dev) == IOT_JOBS_REJECT);
    doc = good_doc(); doc.url = NULL;
    CHECK(plan(&doc, &dev) == IOT_JOBS_REJECT);
    doc = good_doc(); doc.url = "http://example.com/firmware.bin";
    CHECK(plan(&doc, &dev) == IOT_JOBS_REJECT);
}

static void test_wrong_family_rejected_even_if_version_matches(void)
{
    /* A family mismatch must never be reported as SUCCEEDED. */
    iot_jobs_document_t doc = good_doc();
    doc.family = "multispeq";
    iot_jobs_device_t dev = device_on("1.3.0");
    CHECK(plan(&doc, &dev) == IOT_JOBS_REJECT);
}

int main(void)
{
    test_versions_equal();
    test_install_when_behind();
    test_succeed_without_flash_when_current();
    test_defer_while_image_unconfirmed();
    test_fail_after_rollback();
    test_reject_unusable_documents();
    test_wrong_family_rejected_even_if_version_matches();

    if (s_failures != 0) {
        fprintf(stderr, "%u check(s) failed\n", s_failures);
        return 1;
    }
    printf("iot_jobs plan tests passed\n");
    return 0;
}
