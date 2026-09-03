#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "ambit_announcement.h"

typedef struct {
    ambit_announcement_tuple_t slots[AMBIT_ANNOUNCEMENT_SLOTS];
    size_t loads;
    size_t saves;
    bool fail_save;
} fake_store_t;

static esp_err_t fake_load(void *ctx, size_t slot,
                           ambit_announcement_tuple_t *out)
{
    fake_store_t *store = ctx;
    assert(slot < AMBIT_ANNOUNCEMENT_SLOTS);
    store->loads++;
    *out = store->slots[slot];
    return out->valid ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static esp_err_t fake_save(void *ctx, size_t slot,
                           const ambit_announcement_tuple_t *tuple)
{
    fake_store_t *store = ctx;
    assert(slot < AMBIT_ANNOUNCEMENT_SLOTS);
    store->saves++;
    if (store->fail_save) return ESP_FAIL;
    store->slots[slot] = *tuple;
    return ESP_OK;
}

static ambit_announcement_tuple_t tuple(const char *sensor,
                                        const char *firmware,
                                        uint32_t cal)
{
    ambit_announcement_tuple_t value = {.valid = true, .cal_version = cal};
    snprintf(value.sensor_id, sizeof value.sensor_id, "%s", sensor);
    snprintf(value.firmware, sizeof value.firmware, "%s", firmware);
    return value;
}

int main(void)
{
    fake_store_t store = {0};
    store.slots[0] = tuple("AA", "1.0.0", 1);
    ambit_announcement_store_port_t port = {
        .load = fake_load,
        .save = fake_save,
        .ctx = &store,
    };
    ambit_announcement_tracker_t tracker;
    ambit_announcement_init(&tracker, port);

    const char *attached[AMBIT_ANNOUNCEMENT_SLOTS] = {"AA", NULL, NULL, NULL};
    bool unchanged = false;
    ambit_announcement_tuple_t candidate = tuple("AA", "1.0.0", 1);
    size_t slot = ambit_announcement_select(&tracker, &candidate, attached,
                                            &unchanged);
    assert(slot == 0U);
    assert(unchanged);
    assert(store.loads == AMBIT_ANNOUNCEMENT_SLOTS);

    candidate = tuple("AA", "1.0.1", 1);
    slot = ambit_announcement_select(&tracker, &candidate, attached, &unchanged);
    assert(slot == 0U);
    assert(!unchanged);
    assert(ambit_announcement_commit(&tracker, slot, &candidate) == ESP_OK);
    assert(store.saves == 1U);

    /* A new tracker models a reboot: persisted state still suppresses the
     * unchanged reconnect announcement. */
    ambit_announcement_init(&tracker, port);
    slot = ambit_announcement_select(&tracker, &candidate, attached, &unchanged);
    assert(slot == 0U);
    assert(unchanged);

    /* Failed persistence may duplicate later, but must never update volatile
     * dedupe state and suppress the only durable announcement. */
    candidate = tuple("AA", "1.0.2", 2);
    store.fail_save = true;
    assert(ambit_announcement_commit(&tracker, 0U, &candidate) == ESP_FAIL);
    slot = ambit_announcement_select(&tracker, &candidate, attached, &unchanged);
    assert(slot == 0U);
    assert(!unchanged);

    puts("ambit announcement tracker tests: ok");
    return 0;
}
