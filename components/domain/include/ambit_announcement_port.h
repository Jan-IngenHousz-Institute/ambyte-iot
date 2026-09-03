#ifndef AMBYTE_AMBIT_ANNOUNCEMENT_PORT_H
#define AMBYTE_AMBIT_ANNOUNCEMENT_PORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AMBIT_ANNOUNCEMENT_SLOTS 4U
#define AMBIT_ANNOUNCEMENT_SENSOR_ID_CAP 18U
#define AMBIT_ANNOUNCEMENT_FIRMWARE_CAP 32U

typedef struct {
    bool valid;
    char sensor_id[AMBIT_ANNOUNCEMENT_SENSOR_ID_CAP];
    char firmware[AMBIT_ANNOUNCEMENT_FIRMWARE_CAP];
    uint32_t cal_version;
} ambit_announcement_tuple_t;

typedef esp_err_t (*ambit_announcement_load_fn)(
    void *ctx, size_t slot, ambit_announcement_tuple_t *out);
typedef esp_err_t (*ambit_announcement_save_fn)(
    void *ctx, size_t slot, const ambit_announcement_tuple_t *tuple);

typedef struct {
    ambit_announcement_load_fn load;
    ambit_announcement_save_fn save;
    void *ctx;
} ambit_announcement_store_port_t;

#ifdef __cplusplus
}
#endif

#endif
