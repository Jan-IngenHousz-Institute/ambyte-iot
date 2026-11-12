/* Gated SD preflight for the AMBIT ROM flash (see ambit_flash_preflight.h). */
#include "ambit_flash_preflight.h"

#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>

#include "ambit_fw_manifest.h"
#include "esp_log.h"
#include "sd_card.h"
#include "sd_diag.h"

/* Verification build only: `evq_hil fault io ambit_flash …` (LAST include). */
#define EVQ_HIL_WRITER SD_DIAG_W_AMBIT_FLASH
#include "evq_hil_io_w.h"

#define TAG "ambit_flash"

long ambit_flash_region_size(const char *dir, const char *fname)
{
    char path[192];
    snprintf(path, sizeof path, "%s/%s", dir, fname);
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return 0;
    }
    long sz = -1;
    if (fseek(f, 0, SEEK_END) == 0) sz = ftell(f);
    fclose(f);
    return (sz > 0) ? sz : 0;
}

esp_err_t ambit_flash_preflight(const char *root, const char *dir, const char *const *fnames, size_t n)
{
    if (!sdcard_io_begin()) {
        ESP_LOGE(TAG, "SD unavailable: target untouched");
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t rc = ESP_OK;
    /* Recovery folders were historically installed by the factory SD image.
     * Some field cards predate that layout, which made remote recovery
     * impossible even when operators could stage the four exact files. Create
     * only the canonical root/version directories here, before touching the
     * target. Missing files still fail closed below. */
    if (mkdir(root, 0777) != 0 && errno != EEXIST) {
        sd_diag_fault(SD_DIAG_W_AMBIT_FLASH, SD_DIAG_OP_MKDIR, errno);
        ESP_LOGE(TAG, "cannot create %s", root);
        rc = ESP_FAIL;
    } else if (mkdir(dir, 0777) != 0 && errno != EEXIST) {
        sd_diag_fault(SD_DIAG_W_AMBIT_FLASH, SD_DIAG_OP_MKDIR, errno);
        ESP_LOGE(TAG, "cannot create recovery directory %s", dir);
        rc = ESP_FAIL;
    } else {
        /* Fail fast: all region files must be present + non-empty BEFORE the
         * chip is touched, so a missing file never half-flashes it. */
        for (size_t i = 0; i < n; i++) {
            if (ambit_flash_region_size(dir, fnames[i]) == 0) {
                ESP_LOGE(TAG, "missing/empty %s/%s — need all %u region files", dir, fnames[i], (unsigned)n);
                rc = ESP_ERR_NOT_FOUND;
                break;
            }
        }
        /* Release-manifest gate (ambit_fw_manifest.h): a folder carrying
         * manifest.json (every folder the remote fetch writes) is flashed only
         * when each region's size and SHA-256 match it, so a download cut short by a
         * reset can never put a truncated image on the chip. Legacy hand-staged
         * folders (no manifest) keep the present-and-non-empty rule above. */
        if (rc == ESP_OK) {
            const char *why = NULL;
            ambit_fw_dir_t g = ambit_fw_dir_check(dir, true, &why);
            if (g == AMBIT_FW_DIR_REJECTED) {
                ESP_LOGE(TAG, "%s fails its manifest.json (%s); target untouched", dir, why);
                rc = ESP_ERR_INVALID_CRC;
            } else if (g == AMBIT_FW_DIR_UNREADABLE) {
                ESP_LOGE(TAG, "%s: SD read error verifying manifest.json (%s); target untouched", dir, why);
                rc = ESP_FAIL;
            } else if (g == AMBIT_FW_DIR_VERIFIED) {
                ESP_LOGW(TAG, "%s matches its manifest.json (size + SHA-256)", dir);
            }
        }
    }
    sdcard_io_end();
    return rc;
}
