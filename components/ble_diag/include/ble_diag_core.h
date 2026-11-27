#pragma once

/* Pure parts of the bench BLE diagnostics service (contract v0,
 * docs/bench/ble-diagnostics.md): snapshot double buffer + seq, page framing,
 * page_offset parsing, and the meta/live JSON renderers. No ESP-IDF, no
 * locks: the caller (ble_diag.c) owns every lock, the host test
 * (tests/ble_diag_host.c) drives these directly under ASan/UBSan. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The heartbeat sample object is built into a PAYLOAD_V3_TELEMETRY_CAP (8192 B)
 * buffer including its NUL; the snapshot holds the JSON without it. */
#define BLE_DIAG_SNAPSHOT_CAP  8192u
/* page = 4-byte LE seq + up to 240 data bytes = 244: fits one ATT read
 * response at the contract's MTU 247 (MTU - 1 opcode byte = 246). */
#define BLE_DIAG_PAGE_DATA_MAX 240u
#define BLE_DIAG_PAGE_MAX      (4u + BLE_DIAG_PAGE_DATA_MAX)
/* Contract ceilings: a notification carries at most MTU - 3 = 244 bytes. */
#define BLE_DIAG_META_MAX      180u
#define BLE_DIAG_LIVE_MAX      200u
/* esp_app_desc_t.version is char[32]. */
#define BLE_DIAG_FW_MAX        31u

typedef struct {
    uint8_t *buf[2];   /* each `cap` bytes; buf[active] is the served copy */
    size_t   cap;
    uint8_t  active;
    uint32_t len;      /* bytes of buf[active]; 0 = no heartbeat built yet */
    uint32_t seq;      /* 0 = none yet; bumps per published heartbeat, skips 0 */
    int64_t  built_ms; /* uptime when the served copy was published */
} ble_diag_snap_t;

/* Copy json into the STAGING (inactive) buffer. False, nothing staged, if
 * len is 0 or exceeds cap. Writers must be serialised by the caller; readers
 * may keep reading buf[active] concurrently (only the publish swaps). */
bool ble_diag_snap_stage(ble_diag_snap_t *s, const char *json, size_t len);
/* Make the staged copy the served one: swap, set len/built_ms, bump seq.
 * O(1): the caller holds its reader lock across this only. */
void ble_diag_snap_publish(ble_diag_snap_t *s, size_t len, int64_t now_ms);
/* One `page` characteristic value: 4-byte LE seq, then min(240, len-offset)
 * bytes from offset (none at/after the end). Returns total bytes written
 * (4..BLE_DIAG_PAGE_MAX). Copies at most 240 bytes: safe under a spinlock. */
size_t ble_diag_snap_page(const ble_diag_snap_t *s, uint32_t offset,
                          uint8_t out[BLE_DIAG_PAGE_MAX]);

/* page_offset write: exactly 4 bytes, little-endian u32. */
bool ble_diag_parse_offset(const uint8_t *data, size_t len, uint32_t *offset);

/* `meta` JSON. age_ms < 0 renders null (no heartbeat built yet). fw is
 * sanitised to [0-9A-Za-z._+-], at most 31 chars, so no escaping can be needed.
 * Returns the length, or -1 if it would not fit cap or BLE_DIAG_META_MAX. */
int ble_diag_render_meta(char *out, size_t cap, uint32_t seq, uint32_t len,
                         int64_t age_ms, const char *fw, const char *mac);

typedef struct {
    bool        have;      /* false: no charger read cached yet */
    int64_t     age_ms;    /* since the cached frame was captured */
    uint16_t    vin_mv, vsys_mv, vbat_mv, iin_ma, ichg_ma;
    uint8_t     status;    /* REG0C */
    uint8_t     fault;     /* REG0D */
    bool        valid;     /* the read's verdict was ESP_OK */
    const char *reason;    /* power_read_reason_name(); [a-z_] only */
} ble_diag_live_t;

/* `live` JSON. !have renders age_ms null, zero values, valid false. Returns
 * the length, or -1 if it would not fit cap or BLE_DIAG_LIVE_MAX. */
int ble_diag_render_live(char *out, size_t cap, const ble_diag_live_t *live);

/* "Ambyte-XXXX" from the last two MAC bytes (uppercase), and the
 * "AA:BB:CC:DD:EE:FF" form used in meta. */
void ble_diag_format_name(const uint8_t mac[6], char out[12]);
void ble_diag_format_mac(const uint8_t mac[6], char out[18]);

#ifdef __cplusplus
}
#endif
