#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_mac.h"
#include "fleet_jitter.h"

#define ARRAY_LEN(values) (sizeof(values) / sizeof((values)[0]))
#define TEST_MAC_READ_ERROR 0x701

typedef struct {
    uint8_t mac[6];
    uint32_t hash;
    uint32_t fleet_slot;
    uint32_t reboot_slot;
} jitter_vector_t;

static const jitter_vector_t s_vectors[] = {
    /* Representative slots in the first, middle, and last regions of 0..899. */
    {{0x02, 0x00, 0x00, 0x00, 0x00, 0x41}, 3072580284U,  84U, 84U},
    {{0x02, 0x00, 0x00, 0x00, 0x00, 0x21}, 2535696476U, 476U, 26U},
    {{0x02, 0x00, 0x00, 0x00, 0x00, 0x01}, 1998812668U, 868U, 58U},
};

static uint8_t s_fake_mac[6];
static esp_err_t s_fake_mac_result = ESP_OK;
static unsigned s_failures = 0;

#define CHECK(expression)                                                        \
    do {                                                                         \
        if (!(expression)) {                                                     \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n",                       \
                    __FILE__, __LINE__, #expression);                            \
            s_failures++;                                                        \
        }                                                                        \
    } while (0)

esp_err_t esp_read_mac(uint8_t *mac, esp_mac_type_t type)
{
    CHECK(type == ESP_MAC_WIFI_STA);
    if (s_fake_mac_result != ESP_OK) {
        return s_fake_mac_result;
    }
    memcpy(mac, s_fake_mac, sizeof s_fake_mac);
    return ESP_OK;
}

static void test_known_vectors(void)
{
    for (size_t i = 0; i < ARRAY_LEN(s_vectors); i++) {
        uint32_t slot = UINT32_MAX;
        CHECK(fleet_jitter_slot_for_mac(s_vectors[i].mac, 900U, &slot) == ESP_OK);
        CHECK(slot == s_vectors[i].fleet_slot);

        slot = UINT32_MAX;
        CHECK(fleet_jitter_slot_for_mac(s_vectors[i].mac, 90U, &slot) == ESP_OK);
        CHECK(slot == s_vectors[i].reboot_slot);

        /* Each vector's hash is below UINT32_MAX, so this modulo exposes the
         * complete production FNV-1a result as well as the selected slots. */
        slot = 0;
        CHECK(fleet_jitter_slot_for_mac(s_vectors[i].mac, UINT32_MAX, &slot) == ESP_OK);
        CHECK(slot == s_vectors[i].hash);
    }
}

static void test_bounds(void)
{
    const uint32_t slot_counts[] = {1U, 2U, 90U, 900U, UINT32_MAX};
    uint8_t mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x00};

    for (uint32_t suffix = 0; suffix <= UINT8_MAX; suffix++) {
        mac[5] = (uint8_t)suffix;
        for (size_t i = 0; i < ARRAY_LEN(slot_counts); i++) {
            uint32_t slot = UINT32_MAX;
            CHECK(fleet_jitter_slot_for_mac(mac, slot_counts[i], &slot) == ESP_OK);
            CHECK(slot < slot_counts[i]);
        }
    }
}

static void test_invalid_arguments(void)
{
    uint8_t mac[6] = {0};
    uint32_t slot = 123U;

    CHECK(fleet_jitter_slot_for_mac(NULL, 900U, &slot) == ESP_ERR_INVALID_ARG);
    CHECK(fleet_jitter_slot_for_mac(mac, 0U, &slot) == ESP_ERR_INVALID_ARG);
    CHECK(fleet_jitter_slot_for_mac(mac, 900U, NULL) == ESP_ERR_INVALID_ARG);
    CHECK(fleet_jitter_slot_for_sta_mac(0U, &slot) == ESP_ERR_INVALID_ARG);
    CHECK(fleet_jitter_slot_for_sta_mac(900U, NULL) == ESP_ERR_INVALID_ARG);
}

static void test_sta_mac_read(void)
{
    memcpy(s_fake_mac, s_vectors[1].mac, sizeof s_fake_mac);
    s_fake_mac_result = ESP_OK;
    uint32_t slot = UINT32_MAX;
    CHECK(fleet_jitter_slot_for_sta_mac(900U, &slot) == ESP_OK);
    CHECK(slot == s_vectors[1].fleet_slot);

    /* The helper propagates the hardware error and leaves policy/fallback to
     * the caller; sync_runner initializes its reboot slot to zero. */
    s_fake_mac_result = TEST_MAC_READ_ERROR;
    slot = 0U;
    CHECK(fleet_jitter_slot_for_sta_mac(90U, &slot) == TEST_MAC_READ_ERROR);
    CHECK(slot == 0U);
}

int main(void)
{
    test_known_vectors();
    test_bounds();
    test_invalid_arguments();
    test_sta_mac_read();

    if (s_failures != 0U) {
        fprintf(stderr, "fleet_jitter: %u failure(s)\n", s_failures);
        return 1;
    }
    puts("fleet_jitter: all tests passed");
    return 0;
}
