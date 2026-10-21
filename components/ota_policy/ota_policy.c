#include "ota_policy.h"
#include <stdio.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_image_format.h"
#include "esp_ota_ops.h"
#include "sdkconfig.h"
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

#if !CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT || !CONFIG_SECURE_SIGNED_APPS_RSA_SCHEME
#error "OTA policy requires S3 signed-update RSA verification"
#endif
#if CONFIG_SECURE_BOOT || CONFIG_SECURE_FLASH_ENC_ENABLED || CONFIG_SECURE_DISABLE_ROM_DL_MODE || CONFIG_SECURE_ENABLE_SECURE_ROM_DL_MODE
#error "A5 must preserve physical recovery and must not burn security eFuses"
#endif

static void budget(const char *phase)
{
#ifdef ESP_PLATFORM
    unsigned caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    ESP_LOGI("ota_policy", "budget phase=%s free=%u largest=%u min=%u stack=%u", phase,
             (unsigned)heap_caps_get_free_size(caps),
             (unsigned)heap_caps_get_largest_free_block(caps),
             (unsigned)heap_caps_get_minimum_free_size(caps),
             (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)));
#else
    (void)phase;
#endif
}

bool ota_policy_read_manifest(const esp_partition_t *p, ota_manifest_t *out)
{
    uint8_t prefix[OTA_MANIFEST_OFFSET + OTA_MANIFEST_BYTES];
    if (!p || p->size < sizeof prefix || esp_partition_read(p, 0, prefix, sizeof prefix) != ESP_OK)
        return false;
    /* The fixed descriptor is meaningful only inside the first data segment. */
    uint32_t segment_len = (uint32_t)prefix[28] | (uint32_t)prefix[29] << 8 |
                          (uint32_t)prefix[30] << 16 | (uint32_t)prefix[31] << 24;
    if (prefix[0] != 0xe9 || prefix[1] == 0 || prefix[1] > 16 || prefix[12] != 9 || prefix[13] ||
        segment_len < sizeof(esp_app_desc_t) + OTA_MANIFEST_BYTES ||
        prefix[32] != 0x32 || prefix[33] != 0x54 || prefix[34] != 0xcd || prefix[35] != 0xab)
        return false;
    return ota_manifest_decode(prefix + OTA_MANIFEST_OFFSET, OTA_MANIFEST_BYTES, out);
}

esp_err_t ota_image_admit(const esp_partition_t *p, size_t received, char *reason, size_t cap)
{
    const char *failure = NULL;
    esp_image_metadata_t image = {0};
    ota_manifest_t r, c;
    if (!p || received > p->size || received < OTA_MANIFEST_OFFSET + OTA_MANIFEST_BYTES)
        failure = "signature_invalid";
    else {
        /* Limit the SDK verifier to bytes actually received, so a stale tail in
         * the inactive slot cannot supply a missing signature. IDF includes the
         * complete signature-sector structure in metadata.image_len. */
        esp_partition_pos_t pos = {.offset = p->address, .size = received};
        budget("verify_begin");
        if (esp_image_verify(ESP_IMAGE_VERIFY, &pos, &image) != ESP_OK ||
            image.image_len > received || !image.image_len ||
            ((image.image_len + 4095U) & ~4095U) > received)
            failure = "signature_invalid";
        else if (!ota_policy_read_manifest(esp_ota_get_running_partition(), &r))
            failure = "running_manifest_invalid";
        else if (!ota_policy_read_manifest(p, &c))
            failure = "candidate_manifest_invalid";
        else {
            /* A5 has one unchanged format and no version marker. This provider
             * must become a durable /evstore union before A6 writes version2. */
            failure = ota_manifest_decide(&r, &c, UINT32_C(1) << r.written);
        }
        budget("verify_end");
    }
    if (reason && cap) snprintf(reason, cap, "%s", failure ? failure : "");
    return failure ? ESP_ERR_OTA_VALIDATE_FAILED : ESP_OK;
}
