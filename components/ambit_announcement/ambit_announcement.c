#include "ambit_announcement.h"

#include <string.h>

static void load_once(ambit_announcement_tracker_t *tracker)
{
    if (tracker->loaded) return;
    tracker->loaded = true;
    if (tracker->store.load == NULL) return;
    for (size_t i = 0; i < AMBIT_ANNOUNCEMENT_SLOTS; ++i) {
        ambit_announcement_tuple_t tuple = {0};
        if (tracker->store.load(tracker->store.ctx, i, &tuple) == ESP_OK &&
            tuple.valid) {
            tracker->slots[i] = tuple;
        }
    }
}

static bool same_tuple(const ambit_announcement_tuple_t *a,
                       const ambit_announcement_tuple_t *b)
{
    return a->valid && b->valid &&
           strcmp(a->sensor_id, b->sensor_id) == 0 &&
           strcmp(a->firmware, b->firmware) == 0 &&
           a->cal_version == b->cal_version;
}

static bool sensor_attached(
    const char *sensor,
    const char *const attached_sensor_ids[AMBIT_ANNOUNCEMENT_SLOTS])
{
    for (size_t i = 0; i < AMBIT_ANNOUNCEMENT_SLOTS; ++i) {
        if (attached_sensor_ids[i] != NULL &&
            strcmp(sensor, attached_sensor_ids[i]) == 0) return true;
    }
    return false;
}

void ambit_announcement_init(ambit_announcement_tracker_t *tracker,
                             ambit_announcement_store_port_t store)
{
    if (tracker == NULL) return;
    memset(tracker, 0, sizeof *tracker);
    tracker->store = store;
}

size_t ambit_announcement_select(
    ambit_announcement_tracker_t *tracker,
    const ambit_announcement_tuple_t *candidate,
    const char *const attached_sensor_ids[AMBIT_ANNOUNCEMENT_SLOTS],
    bool *unchanged)
{
    if (unchanged != NULL) *unchanged = false;
    if (tracker == NULL || candidate == NULL || !candidate->valid ||
        attached_sensor_ids == NULL) return 0U;
    load_once(tracker);

    size_t empty = AMBIT_ANNOUNCEMENT_SLOTS;
    for (size_t i = 0; i < AMBIT_ANNOUNCEMENT_SLOTS; ++i) {
        if (!tracker->slots[i].valid) {
            if (empty == AMBIT_ANNOUNCEMENT_SLOTS) empty = i;
            continue;
        }
        if (strcmp(tracker->slots[i].sensor_id, candidate->sensor_id) == 0) {
            if (unchanged != NULL) *unchanged = same_tuple(&tracker->slots[i], candidate);
            return i;
        }
    }
    if (empty != AMBIT_ANNOUNCEMENT_SLOTS) return empty;
    for (size_t n = 0; n < AMBIT_ANNOUNCEMENT_SLOTS; ++n) {
        const size_t i = (tracker->evict_start + n) % AMBIT_ANNOUNCEMENT_SLOTS;
        if (!sensor_attached(tracker->slots[i].sensor_id, attached_sensor_ids)) return i;
    }
    return tracker->evict_start % AMBIT_ANNOUNCEMENT_SLOTS;
}

esp_err_t ambit_announcement_commit(
    ambit_announcement_tracker_t *tracker, size_t slot,
    const ambit_announcement_tuple_t *candidate)
{
    if (tracker == NULL || candidate == NULL || !candidate->valid ||
        slot >= AMBIT_ANNOUNCEMENT_SLOTS || tracker->store.save == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = tracker->store.save(tracker->store.ctx, slot, candidate);
    if (err != ESP_OK) return err;
    if (tracker->slots[slot].valid &&
        strcmp(tracker->slots[slot].sensor_id, candidate->sensor_id) != 0) {
        tracker->evict_start = (slot + 1U) % AMBIT_ANNOUNCEMENT_SLOTS;
    }
    tracker->slots[slot] = *candidate;
    return ESP_OK;
}
