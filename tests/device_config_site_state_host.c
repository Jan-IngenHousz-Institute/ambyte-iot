/* SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Fault-injection harness for the real device_config.c site-state path. The
 * fake NVS models a staged value and an atomically committed durable value;
 * longjmp marks power cuts at each boundary, after which fake_reset() discards
 * uncommitted state exactly as a reboot does. */

#include <assert.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "device_config.h"
#include "nvs.h"
#include "timezone.h"

#define SITE_SIZE 88

typedef enum {
    FAULT_NONE,
    FAIL_SITE_SET_FULL,
    FAIL_SITE_COMMIT_FULL,
    RESET_AFTER_SITE_SET,
    RESET_BEFORE_SITE_COMMIT,
    RESET_AFTER_SITE_COMMIT,
} fault_t;

static uint8_t durable_site[SITE_SIZE];
static size_t durable_site_len;
static bool durable_site_present;
static uint8_t staged_site[SITE_SIZE];
static size_t staged_site_len;
static bool staged_site_present;
static bool legacy_lat_present;
static bool legacy_lon_present;
static bool legacy_deployment_present;
static double legacy_lat;
static double legacy_lon;
static char legacy_deployment[64];
static fault_t fault;
static jmp_buf power_cut;
static bool power_cut_armed;
static unsigned site_set_calls;
static unsigned commit_calls;

static void fake_clear(void)
{
    durable_site_present = false;
    staged_site_present = false;
    legacy_lat_present = false;
    legacy_lon_present = false;
    legacy_deployment_present = false;
    durable_site_len = staged_site_len = 0;
    legacy_lat = legacy_lon = 0;
    legacy_deployment[0] = '\0';
    fault = FAULT_NONE;
    power_cut_armed = false;
    site_set_calls = commit_calls = 0;
}

static void fake_reset(void)
{
    staged_site_present = false;
    staged_site_len = 0;
    fault = FAULT_NONE;
    power_cut_armed = false;
}

static void fake_seed_legacy(double lat, double lon, const char *deployment)
{
    legacy_lat_present = true;
    legacy_lon_present = true;
    legacy_deployment_present = true;
    legacy_lat = lat;
    legacy_lon = lon;
    strcpy(legacy_deployment, deployment);
}

static void fake_corrupt_blob(unsigned version, bool bad_termination)
{
    memset(durable_site, 0, sizeof(durable_site));
    memcpy(durable_site, "AMST", 4);
    durable_site[4] = (uint8_t)version;
    durable_site[5] = 7;
    durable_site[6] = 3;
    if (bad_termination) {
        memcpy(durable_site + 24, "abcX", 4);
    }
    durable_site_len = sizeof(durable_site);
    durable_site_present = true;
}

esp_err_t nvs_open(const char *namespace_name, nvs_open_mode_t open_mode,
                   nvs_handle_t *out_handle)
{
    (void)namespace_name;
    (void)open_mode;
    *out_handle = 1;
    return ESP_OK;
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out_value,
                       size_t *length)
{
    (void)handle;
    const void *value = NULL;
    size_t value_len = 0;
    if (strcmp(key, "site_state") == 0) {
        if (staged_site_present) {
            value = staged_site;
            value_len = staged_site_len;
        } else if (durable_site_present) {
            value = durable_site;
            value_len = durable_site_len;
        }
    } else if (strcmp(key, "lat") == 0 && legacy_lat_present) {
        value = &legacy_lat;
        value_len = sizeof(legacy_lat);
    } else if (strcmp(key, "lon") == 0 && legacy_lon_present) {
        value = &legacy_lon;
        value_len = sizeof(legacy_lon);
    }
    if (value == NULL) return ESP_ERR_NVS_NOT_FOUND;
    if (out_value == NULL || *length < value_len) {
        *length = value_len;
        return ESP_ERR_NVS_INVALID_LENGTH;
    }
    memcpy(out_value, value, value_len);
    *length = value_len;
    return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value,
                       size_t length)
{
    (void)handle;
    if (strcmp(key, "site_state") != 0) return ESP_OK;
    site_set_calls++;
    if (fault == FAIL_SITE_SET_FULL) return ESP_ERR_NVS_NOT_ENOUGH_SPACE;
    assert(length == SITE_SIZE);
    memcpy(staged_site, value, length);
    staged_site_len = length;
    staged_site_present = true;
    if (fault == RESET_AFTER_SITE_SET) {
        assert(power_cut_armed);
        longjmp(power_cut, 1);
    }
    return ESP_OK;
}

esp_err_t nvs_get_str(nvs_handle_t handle, const char *key, char *out_value,
                      size_t *length)
{
    (void)handle;
    if (strcmp(key, "deployment") != 0 || !legacy_deployment_present) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    size_t needed = strlen(legacy_deployment) + 1;
    if (out_value == NULL || *length < needed) {
        *length = needed;
        return ESP_ERR_NVS_INVALID_LENGTH;
    }
    memcpy(out_value, legacy_deployment, needed);
    *length = needed;
    return ESP_OK;
}

esp_err_t nvs_set_str(nvs_handle_t handle, const char *key, const char *value)
{
    (void)handle;
    (void)key;
    (void)value;
    return ESP_OK;
}

esp_err_t nvs_get_u32(nvs_handle_t handle, const char *key, uint32_t *out_value)
{
    (void)handle;
    (void)key;
    (void)out_value;
    return ESP_ERR_NVS_NOT_FOUND;
}

esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key)
{
    (void)handle;
    (void)key;
    return ESP_ERR_NVS_NOT_FOUND;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    (void)handle;
    commit_calls++;
    if (!staged_site_present) return ESP_OK;
    if (fault == RESET_BEFORE_SITE_COMMIT) {
        assert(power_cut_armed);
        longjmp(power_cut, 1);
    }
    if (fault == FAIL_SITE_COMMIT_FULL) return ESP_ERR_NVS_NOT_ENOUGH_SPACE;
    memcpy(durable_site, staged_site, staged_site_len);
    durable_site_len = staged_site_len;
    durable_site_present = true;
    staged_site_present = false;
    if (fault == RESET_AFTER_SITE_COMMIT) {
        assert(power_cut_armed);
        longjmp(power_cut, 1);
    }
    return ESP_OK;
}

esp_err_t timezone_canonicalize(const char *input, char *output,
                                size_t output_size)
{
    if (input == NULL || output == NULL || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t len = strlen(input);
    if (len + 1 > output_size) return ESP_ERR_INVALID_SIZE;
    memcpy(output, input, len + 1);
    return ESP_OK;
}

static void assert_tuple(double lat, double lon, const char *deployment)
{
    double got_lat = 0, got_lon = 0;
    char got_deployment[64] = "";
    assert(device_config_get_location(&got_lat, &got_lon, got_deployment,
                                      sizeof(got_deployment)) == ESP_OK);
    assert(got_lat == lat);
    assert(got_lon == lon);
    assert(strcmp(got_deployment, deployment) == 0);
}

static void seed_old_blob(void)
{
    fault = FAULT_NONE;
    assert(device_config_set_location(10.25, -20.5, "old-site") == ESP_OK);
    fake_reset();
    assert_tuple(10.25, -20.5, "old-site");
}

static void test_full_failures_never_mix(void)
{
    const fault_t failures[] = {FAIL_SITE_SET_FULL, FAIL_SITE_COMMIT_FULL};
    for (size_t i = 0; i < sizeof(failures) / sizeof(failures[0]); i++) {
        fake_clear();
        seed_old_blob();
        fault = failures[i];
        assert(device_config_set_location(51.5, 4.75, "new-site") ==
               ESP_ERR_NVS_NOT_ENOUGH_SPACE);
        if (failures[i] == FAIL_SITE_COMMIT_FULL) {
            /* NVS may offer read-your-uncommitted-write, but it is still one
             * complete new tuple rather than mixed fields. */
            assert_tuple(51.5, 4.75, "new-site");
        } else {
            assert_tuple(10.25, -20.5, "old-site");
        }
        fake_reset();
        assert_tuple(10.25, -20.5, "old-site");
    }
}

static void test_reset_boundaries_never_mix(void)
{
    const fault_t cuts[] = {
        RESET_AFTER_SITE_SET,
        RESET_BEFORE_SITE_COMMIT,
        RESET_AFTER_SITE_COMMIT,
    };
    for (size_t i = 0; i < sizeof(cuts) / sizeof(cuts[0]); i++) {
        fake_clear();
        seed_old_blob();
        fault = cuts[i];
        power_cut_armed = true;
        if (setjmp(power_cut) == 0) {
            (void)device_config_set_location(51.5, 4.75, "new-site");
            assert(!"power cut did not fire");
        }
        fake_reset();
        if (cuts[i] == RESET_AFTER_SITE_COMMIT) {
            assert_tuple(51.5, 4.75, "new-site");
        } else {
            assert_tuple(10.25, -20.5, "old-site");
        }
    }
}

static void test_legacy_and_corrupt_fallback(void)
{
    const unsigned versions[] = {1, 99};
    for (size_t i = 0; i < sizeof(versions) / sizeof(versions[0]); i++) {
        fake_clear();
        fake_seed_legacy(52.173, 5.819, "legacy-site");
        fake_corrupt_blob(versions[i], versions[i] == 1);
        fault = FAIL_SITE_SET_FULL; /* reads must not depend on migration */
        assert_tuple(52.173, 5.819, "legacy-site");
        assert(site_set_calls == 0);
    }

    fake_clear();
    fake_seed_legacy(52.173, 5.819, "legacy-site");
    /* A valid deployment-only blob is authoritative; legacy coordinates must
     * not leak into it and manufacture a mixed cross-format tuple. */
    memset(durable_site, 0, sizeof(durable_site));
    memcpy(durable_site, "AMST", 4);
    durable_site[4] = 1;
    durable_site[5] = 4;
    durable_site[6] = 3;
    memcpy(durable_site + 24, "new", 3);
    durable_site_len = sizeof(durable_site);
    durable_site_present = true;
    double lat;
    assert(device_config_get_lat(&lat) == ESP_ERR_NVS_NOT_FOUND);
}

static void test_single_field_setters_are_blob_read_modify_writes(void)
{
    fake_clear();
    fake_seed_legacy(52.173, 5.819, "legacy-site");
    assert(device_config_set_lat(40.0) == ESP_OK);
    assert(device_config_set_lon(6.25) == ESP_OK);
    assert(device_config_set_deployment("migrated-site") == ESP_OK);
    assert(site_set_calls == 3);
    assert(commit_calls == 3);
    fake_reset();
    assert_tuple(40.0, 6.25, "migrated-site");
}

static void test_deployment_validation_matches_wire_contract(void)
{
    fake_clear();
    assert(device_config_set_location(1.0, 2.0, "\xc3\xa9") == ESP_OK);
    fake_reset();
    assert_tuple(1.0, 2.0, "\xc3\xa9");
    assert(device_config_set_deployment("\xff") == ESP_ERR_INVALID_ARG);
}

static void dump_contract_blob(void)
{
    fake_clear();
    assert(device_config_set_location(52.173, 5.819, "greenhouse-a") == ESP_OK);
    assert(durable_site_present && durable_site_len == SITE_SIZE);
    for (size_t i = 0; i < durable_site_len; i++) printf("%02x", durable_site[i]);
    putchar('\n');
}

int main(int argc, char **argv)
{
    fake_clear();
    assert(device_config_init() == ESP_OK);
    if (argc == 2 && strcmp(argv[1], "--dump") == 0) {
        dump_contract_blob();
        return 0;
    }
    test_full_failures_never_mix();
    test_reset_boundaries_never_mix();
    test_legacy_and_corrupt_fallback();
    test_single_field_setters_are_blob_read_modify_writes();
    test_deployment_validation_matches_wire_contract();
    puts("device_config site_state fault tests: OK");
    return 0;
}
