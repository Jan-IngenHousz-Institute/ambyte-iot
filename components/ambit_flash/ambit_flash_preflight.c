/* Gated SD preflight for the AMBIT ROM flash (see ambit_flash_preflight.h). */
#include "ambit_flash_preflight.h"

#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>

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
    }
    sdcard_io_end();
    return rc;
}
