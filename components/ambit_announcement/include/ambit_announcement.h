#ifndef AMBYTE_AMBIT_ANNOUNCEMENT_H
#define AMBYTE_AMBIT_ANNOUNCEMENT_H

#include "ambit_announcement_port.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    ambit_announcement_tuple_t slots[AMBIT_ANNOUNCEMENT_SLOTS];
    ambit_announcement_store_port_t store;
    size_t evict_start;
    bool loaded;
} ambit_announcement_tracker_t;

void ambit_announcement_init(ambit_announcement_tracker_t *tracker,
                             ambit_announcement_store_port_t store);
size_t ambit_announcement_select(
    ambit_announcement_tracker_t *tracker,
    const ambit_announcement_tuple_t *candidate,
    const char *const attached_sensor_ids[AMBIT_ANNOUNCEMENT_SLOTS],
    bool *unchanged);
esp_err_t ambit_announcement_commit(
    ambit_announcement_tracker_t *tracker, size_t slot,
    const ambit_announcement_tuple_t *candidate);

#ifdef __cplusplus
}
#endif

#endif
