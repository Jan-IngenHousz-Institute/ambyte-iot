/* Host test for the pure core of the bench BLE diagnostics service
 * (components/ble_diag/ble_diag_core.c, contract docs/bench/ble-diagnostics.md).
 * Driven by tests/test_ble_diag.py under ASan/UBSan; the pinned SDK's cJSON
 * parses every rendered JSON object. Exit 0 = all checks passed. */

#include "ble_diag_core.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

static int s_failures;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            s_failures++;                                                        \
        }                                                                        \
    } while (0)

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void new_snap(ble_diag_snap_t *s)
{
    memset(s, 0, sizeof *s);
    s->cap = BLE_DIAG_SNAPSHOT_CAP;
    s->buf[0] = malloc(s->cap);
    s->buf[1] = malloc(s->cap);
}

static void free_snap(ble_diag_snap_t *s)
{
    free(s->buf[0]);
    free(s->buf[1]);
}

/* The client loop from the contract: offset 0, 240, 480, ... < len. */
static size_t fetch_all(const ble_diag_snap_t *s, uint8_t *dst, uint32_t *seq_seen,
                        unsigned *pages)
{
    uint8_t page[BLE_DIAG_PAGE_MAX];
    size_t got = 0;
    *pages = 0;
    for (uint32_t off = 0; off < s->len; off += BLE_DIAG_PAGE_DATA_MAX) {
        const size_t n = ble_diag_snap_page(s, off, page);
        CHECK(n >= 4 && n <= BLE_DIAG_PAGE_MAX);
        *seq_seen = le32(page);
        memcpy(dst + got, page + 4, n - 4);
        got += n - 4;
        (*pages)++;
    }
    return got;
}

static void test_empty_snapshot(void)
{
    ble_diag_snap_t s;
    new_snap(&s);
    uint8_t page[BLE_DIAG_PAGE_MAX];
    CHECK(ble_diag_snap_page(&s, 0, page) == 4);
    CHECK(le32(page) == 0);
    free_snap(&s);
}

static void test_roundtrip_sizes(void)
{
    /* 1 byte, exact page multiples, one past, observed sizes, the cap. */
    const size_t sizes[] = {1, 239, 240, 241, 480, 2300, 4400, 8191, 8192};
    char *json = malloc(BLE_DIAG_SNAPSHOT_CAP);
    uint8_t *out = malloc(BLE_DIAG_SNAPSHOT_CAP);
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        ble_diag_snap_t s;
        new_snap(&s);
        const size_t len = sizes[i];
        for (size_t k = 0; k < len; k++) json[k] = (char)('a' + (k * 7 + i) % 26);
        CHECK(ble_diag_snap_stage(&s, json, len));
        ble_diag_snap_publish(&s, len, 1000);
        uint32_t seq = 0;
        unsigned pages = 0;
        const size_t got = fetch_all(&s, out, &seq, &pages);
        CHECK(got == len);
        CHECK(memcmp(out, json, len) == 0);
        CHECK(seq == 1);
        CHECK(pages == (len + BLE_DIAG_PAGE_DATA_MAX - 1) / BLE_DIAG_PAGE_DATA_MAX);
        /* Past the end: seq only. */
        uint8_t page[BLE_DIAG_PAGE_MAX];
        CHECK(ble_diag_snap_page(&s, (uint32_t)len, page) == 4);
        CHECK(ble_diag_snap_page(&s, UINT32_MAX, page) == 4);
        CHECK(le32(page) == 1);
        free_snap(&s);
    }
    free(json);
    free(out);
}

static void test_stage_rejects(void)
{
    ble_diag_snap_t s;
    new_snap(&s);
    char *big = calloc(1, BLE_DIAG_SNAPSHOT_CAP + 1);
    CHECK(!ble_diag_snap_stage(&s, big, BLE_DIAG_SNAPSHOT_CAP + 1));
    CHECK(!ble_diag_snap_stage(&s, big, 0));
    CHECK(!ble_diag_snap_stage(&s, NULL, 10));
    CHECK(s.seq == 0 && s.len == 0);
    free(big);
    free_snap(&s);
}

/* A staged-but-unpublished copy is invisible; publishing swaps atomically and
 * bumps seq, so a client mid-transfer sees seq change and restarts. */
static void test_seq_and_staging(void)
{
    ble_diag_snap_t s;
    new_snap(&s);
    const char *a = "[\"first heartbeat, quite long so it spans pages ...\"]";
    CHECK(ble_diag_snap_stage(&s, a, strlen(a)));
    ble_diag_snap_publish(&s, strlen(a), 10);
    uint8_t page[BLE_DIAG_PAGE_MAX];
    size_t n = ble_diag_snap_page(&s, 0, page);
    CHECK(le32(page) == 1 && n == 4 + strlen(a));

    const char *b = "{\"second\":true}";
    CHECK(ble_diag_snap_stage(&s, b, strlen(b)));
    n = ble_diag_snap_page(&s, 0, page);            /* still serving a */
    CHECK(le32(page) == 1 && n == 4 + strlen(a) && memcmp(page + 4, a, strlen(a)) == 0);
    ble_diag_snap_publish(&s, strlen(b), 20);
    n = ble_diag_snap_page(&s, 0, page);
    CHECK(le32(page) == 2 && n == 4 + strlen(b) && memcmp(page + 4, b, strlen(b)) == 0);
    CHECK(s.built_ms == 20);

    /* Wrap skips 0 ("nothing built yet"). */
    s.seq = UINT32_MAX;
    CHECK(ble_diag_snap_stage(&s, a, strlen(a)));
    ble_diag_snap_publish(&s, strlen(a), 30);
    CHECK(s.seq == 1);
    free_snap(&s);
}

static void test_offset_parse(void)
{
    uint32_t off = 7;
    const uint8_t le[4] = {0xF0, 0x00, 0x00, 0x00};
    CHECK(ble_diag_parse_offset(le, 4, &off) && off == 240);
    const uint8_t big[4] = {0x01, 0x02, 0x03, 0x04};
    CHECK(ble_diag_parse_offset(big, 4, &off) && off == 0x04030201u);
    off = 9;
    CHECK(!ble_diag_parse_offset(le, 3, &off) && off == 9);
    CHECK(!ble_diag_parse_offset(le, 5, &off) && off == 9);
    CHECK(!ble_diag_parse_offset(NULL, 4, &off));
}

static cJSON *parse_obj(const char *s)
{
    cJSON *j = cJSON_Parse(s);
    CHECK(j != NULL && cJSON_IsObject(j));
    return j;
}

static void test_meta(void)
{
    char out[BLE_DIAG_META_MAX + 1];
    int n = ble_diag_render_meta(out, sizeof out, 3, 4123, 1500, "2.22.0",
                                 "28:37:2F:FF:E7:04");
    CHECK(n > 0 && (size_t)n == strlen(out));
    CHECK(strcmp(out, "{\"v\":0,\"seq\":3,\"len\":4123,\"age_ms\":1500,\"fw\":\"2.22.0\","
                      "\"mac\":\"28:37:2F:FF:E7:04\"}") == 0);
    cJSON *j = parse_obj(out);
    cJSON_Delete(j);

    /* No heartbeat yet: age_ms null. */
    n = ble_diag_render_meta(out, sizeof out, 0, 0, -1, "2.22.0", "28:37:2F:FF:E7:04");
    CHECK(n > 0);
    j = parse_obj(out);
    CHECK(cJSON_IsNull(cJSON_GetObjectItem(j, "age_ms")));
    cJSON_Delete(j);

    /* Worst case: every number at its widest, a 31-char version, a hostile
     * version string (quotes/backslashes are dropped, never escaped). */
    n = ble_diag_render_meta(out, sizeof out, UINT32_MAX, UINT32_MAX, INT64_MAX,
                             "v2.22.0-123-g0123456789-dirty+x\"\\\n__overflow__",
                             "FF:FF:FF:FF:FF:FF");
    CHECK(n > 0 && n <= (int)BLE_DIAG_META_MAX);
    j = parse_obj(out);
    const cJSON *fw = cJSON_GetObjectItem(j, "fw");
    CHECK(cJSON_IsString(fw) && strlen(fw->valuestring) <= BLE_DIAG_FW_MAX);
    CHECK(cJSON_GetObjectItem(j, "age_ms")->valuedouble == (double)UINT32_MAX);
    printf("meta worst case: %d bytes (ceiling %u)\n", n, BLE_DIAG_META_MAX);
    cJSON_Delete(j);

    /* Too small a buffer fails closed. */
    char tiny[20];
    CHECK(ble_diag_render_meta(tiny, sizeof tiny, 1, 1, 1, "x", "m") == -1);
}

static void test_live(void)
{
    char out[BLE_DIAG_LIVE_MAX + 1];
    ble_diag_live_t l = {.have = true, .age_ms = 4200, .vin_mv = 5040, .vsys_mv = 4120,
                         .vbat_mv = 4080, .iin_ma = 412, .ichg_ma = 350, .status = 0x36,
                         .fault = 0, .valid = true, .reason = "ok"};
    int n = ble_diag_render_live(out, sizeof out, &l);
    CHECK(n > 0);
    CHECK(strcmp(out, "{\"age_ms\":4200,\"vin_mv\":5040,\"vsys_mv\":4120,\"vbat_mv\":4080,"
                      "\"iin_ma\":412,\"ichg_ma\":350,\"status\":54,\"fault\":0,"
                      "\"valid\":true,\"reason\":\"ok\"}") == 0);

    /* Rejected frame: values still shown, valid false, reason carried. */
    l.valid = false;
    l.reason = "no_input";
    n = ble_diag_render_live(out, sizeof out, &l);
    cJSON *j = parse_obj(out);
    CHECK(cJSON_IsFalse(cJSON_GetObjectItem(j, "valid")));
    CHECK(strcmp(cJSON_GetObjectItem(j, "reason")->valuestring, "no_input") == 0);
    cJSON_Delete(j);

    /* No read cached yet. */
    ble_diag_live_t none = {.have = false, .vin_mv = 999, .valid = true, .reason = "ok"};
    n = ble_diag_render_live(out, sizeof out, &none);
    CHECK(n > 0);
    j = parse_obj(out);
    CHECK(cJSON_IsNull(cJSON_GetObjectItem(j, "age_ms")));
    CHECK(cJSON_IsFalse(cJSON_GetObjectItem(j, "valid")));
    CHECK(cJSON_GetObjectItem(j, "vin_mv")->valueint == 0);
    CHECK(strcmp(cJSON_GetObjectItem(j, "reason")->valuestring, "unavailable") == 0);
    cJSON_Delete(j);

    /* Worst case widths; a foreign reason is replaced, not escaped. */
    ble_diag_live_t worst = {.have = true, .age_ms = INT64_MAX, .vin_mv = UINT16_MAX,
                             .vsys_mv = UINT16_MAX, .vbat_mv = UINT16_MAX,
                             .iin_ma = UINT16_MAX, .ichg_ma = UINT16_MAX, .status = 255,
                             .fault = 255, .valid = false,
                             .reason = "a_reason_that_is_far_too_long_for_the_field"};
    n = ble_diag_render_live(out, sizeof out, &worst);
    CHECK(n > 0 && n <= (int)BLE_DIAG_LIVE_MAX);
    j = parse_obj(out);
    CHECK(strcmp(cJSON_GetObjectItem(j, "reason")->valuestring, "unknown") == 0);
    cJSON_Delete(j);
    worst.reason = "adc_timeout";
    n = ble_diag_render_live(out, sizeof out, &worst);
    CHECK(n > 0 && n <= (int)BLE_DIAG_LIVE_MAX);
    printf("live worst case: %d bytes (ceiling %u)\n", n, BLE_DIAG_LIVE_MAX);
    worst.reason = "bad\"quote";
    n = ble_diag_render_live(out, sizeof out, &worst);
    j = parse_obj(out);
    CHECK(strcmp(cJSON_GetObjectItem(j, "reason")->valuestring, "unknown") == 0);
    cJSON_Delete(j);
}

static void test_names(void)
{
    const uint8_t mac[6] = {0x28, 0x37, 0x2F, 0xFF, 0xE7, 0x04};
    char name[12];
    char m[18];
    ble_diag_format_name(mac, name);
    ble_diag_format_mac(mac, m);
    CHECK(strcmp(name, "Ambyte-E704") == 0);
    CHECK(strcmp(m, "28:37:2F:FF:E7:04") == 0);
}

int main(void)
{
    test_empty_snapshot();
    test_roundtrip_sizes();
    test_stage_rejects();
    test_seq_and_staging();
    test_offset_parse();
    test_meta();
    test_live();
    test_names();
    if (s_failures) {
        fprintf(stderr, "%d check(s) failed\n", s_failures);
        return 1;
    }
    printf("ble_diag_host: all checks passed\n");
    return 0;
}
