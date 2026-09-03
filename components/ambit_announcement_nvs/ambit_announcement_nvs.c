#include "ambit_announcement_nvs.h"

#include <stdio.h>
#include <string.h>

#include "nvs.h"

#define AMBIT_ANNOUNCEMENT_NVS_NS "ambit_ann"

static bool hex_nibble(char c, uint8_t *value)
{
    if (c >= '0' && c <= '9') *value = (uint8_t)(c - '0');
    else if (c >= 'a' && c <= 'f') *value = (uint8_t)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') *value = (uint8_t)(c - 'A' + 10);
    else return false;
    return true;
}

static bool decode(const char *encoded, ambit_announcement_tuple_t *tuple)
{
    const char *first = encoded != NULL ? strchr(encoded, '|') : NULL;
    const char *second = first != NULL ? strchr(first + 1, '|') : NULL;
    if (tuple == NULL || first == NULL || second == NULL ||
        strchr(second + 1, '|') != NULL || first == encoded ||
        strlen(second + 1) != 8U) return false;
    const size_t sensor_len = (size_t)(first - encoded);
    const size_t firmware_len = (size_t)(second - first - 1);
    if (sensor_len >= sizeof tuple->sensor_id ||
        firmware_len >= sizeof tuple->firmware) return false;
    uint32_t cal = 0;
    for (size_t i = 0; i < 8U; ++i) {
        uint8_t nibble = 0;
        if (!hex_nibble(second[1 + i], &nibble)) return false;
        cal = (cal << 4U) | nibble;
    }
    memset(tuple, 0, sizeof *tuple);
    tuple->valid = true;
    memcpy(tuple->sensor_id, encoded, sensor_len);
    memcpy(tuple->firmware, first + 1, firmware_len);
    tuple->cal_version = cal;
    return true;
}

static esp_err_t load_tuple(void *ctx, size_t slot,
                            ambit_announcement_tuple_t *out)
{
    (void)ctx;
    if (out == NULL || slot >= AMBIT_ANNOUNCEMENT_SLOTS) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(AMBIT_ANNOUNCEMENT_NVS_NS, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;
    char key[4], value[64];
    snprintf(key, sizeof key, "a%u", (unsigned)slot);
    size_t len = sizeof value;
    err = nvs_get_str(handle, key, value, &len);
    nvs_close(handle);
    if (err != ESP_OK) return err;
    return decode(value, out) ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static esp_err_t save_tuple(void *ctx, size_t slot,
                            const ambit_announcement_tuple_t *tuple)
{
    (void)ctx;
    if (tuple == NULL || !tuple->valid || slot >= AMBIT_ANNOUNCEMENT_SLOTS)
        return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(AMBIT_ANNOUNCEMENT_NVS_NS, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    char key[4], value[64];
    snprintf(key, sizeof key, "a%u", (unsigned)slot);
    snprintf(value, sizeof value, "%s|%s|%08x", tuple->sensor_id,
             tuple->firmware, (unsigned)tuple->cal_version);
    err = nvs_set_str(handle, key, value);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

ambit_announcement_store_port_t ambit_announcement_nvs_port(void)
{
    return (ambit_announcement_store_port_t){
        .load = load_tuple,
        .save = save_tuple,
    };
}
