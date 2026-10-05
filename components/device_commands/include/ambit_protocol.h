#ifndef AMBYTE_AMBIT_PROTOCOL_H
#define AMBYTE_AMBIT_PROTOCOL_H

/*
 * Ambit-1 ESP binary command IDs and response struct definitions.
 * Struct layouts must match the ambit-1 firmware (ESP32 Xtensa, default alignment).
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Command IDs (cmd_arr[0] in do_esp_cmd) ───────────────────────── */

#define AMBIT_CMD_SET_GAINS           1
#define AMBIT_CMD_SET_CURRENTS        2
#define AMBIT_CMD_CONFIG_DETECTOR    10
#define AMBIT_CMD_RUN_MPF            20
#define AMBIT_CMD_RUN                21
/* Parallel measurement protocol (trigger → poll → fetch). RUN_START takes the
 * same payload as RUN (cmd 21) but the ambit acks and runs into retained
 * buffers; STATUS returns one state byte; FETCH streams the retained arrays. */
#define AMBIT_CMD_RUN_START          22
#define AMBIT_CMD_STATUS             23
#define AMBIT_CMD_FETCH              24
#define AMBIT_CMD_GET_SPEC           31
#define AMBIT_CMD_GET_TEMP           32
#define AMBIT_CMD_GET_INFO           33
#define AMBIT_CMD_GET_TEMP_RAW       34
/* get_spec_raw (AMBIT fw >= 1.2.0, run_esp.cpp case 35): the same AS7341 read
 * as cmd 31 but reported as unscaled counts + the exposure they were taken
 * under + the three-tier calibrated spectrum/PAR. Cmd 31 stays frozen for the
 * deployed fleet; its PAR is spec_coef * legacy integer weights, cmd 35's PAR
 * is par_slope * (par_weight . basic_counts) + par_intercept — a DIFFERENT
 * quantity, never to be mixed under one schema (see ambit.spectrum/2). An
 * older AMBIT logs "Bad command" and sends nothing, so the host sees a
 * timeout, not an error frame. */
#define AMBIT_CMD_GET_SPEC_RAW       35
#define AMBIT_CMD_SET_METADATA       37
#define AMBIT_CMD_ACTINIC             4
#define AMBIT_CMD_BLINK               5
#define AMBIT_CMD_CALIBRATE_BASELINE  6
#define AMBIT_CMD_NVS_SCALAR         17
#define AMBIT_CMD_NVS_ARRAY          18

/* OTA-over-UART (ambit-1 run_esp.cpp cmds 25-28): the ambyte streams a new C3
 * firmware image in CRC16-checked, sequenced chunks; the AMBIT writes it to its
 * spare OTA slot via Update and reboots into it. Each returns a 1-byte status
 * (0 = ok). Orchestrated by components/ambit_ota. */
#define AMBIT_CMD_OTA_BEGIN          25   /* cmd[1..4] = image size (LE u32) */
#define AMBIT_CMD_OTA_DATA           26   /* cmd[1]=len, cmd[2..3]=seq(LE); extra = data + CRC16(LE) */
#define AMBIT_CMD_OTA_END            27   /* finalize + verify + reboot into the new slot */
#define AMBIT_CMD_OTA_ABORT          28   /* discard a partial update */
#define AMBIT_CMD_OTA_CONFIRM        29   /* mark the rebooted image valid (cancel rollback) */
#define AMBIT_OTA_CHUNK_MAX         200   /* max data bytes per OTA_DATA (fits the C3 256 B RX + frame) */

/* ── Info sub-types (cmd_arr[1] for cmd 33) ───────────────────────── */

#define AMBIT_INFO_CALIBRATION  1
#define AMBIT_INFO_FW           2
#define AMBIT_INFO_METADATA     3

/* ── Response structs (match ambit-1 nvs1.h, ESP32 default alignment) */

typedef struct {
    char     ambit_name[20];
    int32_t  mlx_coef[14];
    uint32_t adpd[6];
    float    temp_offset;
    float    temp_slope;
    float    actinic_coef;
    float    spec_coef;
    uint16_t act_50;
    uint16_t act_100;
    uint16_t act_150;
    uint16_t act_200;
    uint16_t act_250;
    float    mlx_emissivity;
    float    sun_coef;
    float    tick_factor;    /* PAM point-period scale (ms tick -> s); MUST match
                              * ambit-1 nvs1.h — omitting it desyncs the framed read */
} ambit_calibration_t;       /* expected ~140 bytes */

typedef struct {
    uint8_t  major;
    uint8_t  minor;
    uint8_t  batch;
    uint32_t size;
    uint64_t mac;
    char     fw_date[12];
    /* hw_rev claims the first formerly-reserved byte (ambit fw >= 0.1.0 writes
     * it; older images never wrote these bytes, so they read as 0 = unknown).
     * Same offsets, same 48-byte total — the wire layout is unchanged. */
    uint8_t  hw_rev;
    char     reserved[11];
    uint8_t  checksum;
} ambit_fw_info_t;           /* expected ~48 bytes */

typedef struct {
    double   lon;
    double   lat;
    float    alt;
    float    acc;
    float    vacc;
    uint32_t time;
    float    x;
    float    y;
    float    z;
    char     info1[200];
    uint16_t eof_mark;
} ambit_metadata_t;          /* expected ~248 bytes */

/* ── Raw response sizes for immediate commands ────────────────────── */

#define AMBIT_RESP_SPEC_SIZE      24   /* 12 × uint16_t (last 4 bytes = float PAR) */
#define AMBIT_RESP_SPEC_RAW_SIZE  80   /* cmd 35 format-1 frame, see ambit_spec_raw_t */
#define AMBIT_RESP_TEMP_SIZE       4   /* 2 × int16_t   (leaf*10, chip*10) */
#define AMBIT_RESP_TEMP_RAW_SIZE  14   /* 7 × int16_t */
#define AMBIT_RESP_STATUS_SIZE     1   /* 1 × uint8_t   (async run state)   */

/* ── cmd 35 (get_spec_raw) frame ──────────────────────────────────────
 * Byte-explicit, little-endian, naturally aligned (u16 on even offsets, f32
 * on multiples of 4) — decoded FIELD BY FIELD, never blitted, so this struct
 * is the host's view and not a wire layout. Offsets (ambit run_esp.cpp):
 *
 *    0  u8   format = 1     bump only for a layout change
 *    1  u8   atime
 *    2  u8   gain_low       as7341_gain_t ORDINAL (n means 0.5 * 2^n), F1-F4
 *    3  u8   gain_high      ORDINAL, F5-F8 + NIR + Clear (the HIGH SMUX read)
 *    4  u16  astep
 *    6  u16  flags          two zones, opposite polarity (below)
 *    8  u16  sat_mask       bit i = channel i at digital full scale
 *   10  u16  clip_mask      bit i = channel i clipped at its dark offset
 *   12  u16  raw[10]        unscaled counts: F1..F8, NIR, Clear
 *   32  f32  chan[10]       goal A: normalised, offset-corrected, spec_sens-scaled
 *   72  f32  par            goal B tier 3: par_slope * par_tier2 + par_intercept
 *   76  f32  par_tier2      goal B tier 2: par_weight . s, before slope/intercept
 *   80  end
 *
 * tint_ms = (atime+1) * (astep+1) * 2.78e-3 (ATIME 99 / ASTEP 499 -> 139 ms);
 * full scale = (atime+1) * (astep+1), so raw[] cannot overflow its u16.
 *
 * flags low byte = CONDITIONS, 1 means needs attention; high byte =
 * CALIBRATION, 1 means confirmed. Zoned so an all-zero word reads as "no
 * fault, nothing confirmed calibrated" — the pessimistic reading a truncated
 * or zeroed frame must produce. A host wanting one "is this PAR trustworthy"
 * test checks that BOTH high bits are set. */
#define AMBIT_SPEC_RAW_FORMAT            1
#define AMBIT_SPEC_RAW_FLAG_SATURATED    (1u << 0)  /* a channel hit digital full scale */
#define AMBIT_SPEC_RAW_FLAG_CLIPPED      (1u << 1)  /* a channel clipped at its dark offset */
#define AMBIT_SPEC_RAW_FLAG_ANALOG_SAT   (1u << 2)  /* AS7341 ASAT (STATUS2) */
#define AMBIT_SPEC_RAW_FLAG_FAULT        (1u << 3)  /* I2C read failed — counts are junk */
#define AMBIT_SPEC_RAW_FLAG_PAR_FIT      (1u << 8)  /* par_weight is an ambit fleet fit (0 = borrowed seed) */
#define AMBIT_SPEC_RAW_FLAG_TIER3_STORED (1u << 9)  /* tier-3 slope/intercept stored on this device */
#define AMBIT_SPEC_RAW_CHANNELS          10

typedef struct {
    uint8_t  format;
    uint8_t  atime;
    uint8_t  gain_low;
    uint8_t  gain_high;
    uint16_t astep;
    uint16_t flags;
    uint16_t sat_mask;
    uint16_t clip_mask;
    uint16_t raw[AMBIT_SPEC_RAW_CHANNELS];
    float    chan[AMBIT_SPEC_RAW_CHANNELS];
    float    par;
    float    par_tier2;
} ambit_spec_raw_t;

/* Integration time in ms from the reported exposure (mirrors the AMBIT's
 * spec_tint_ms); the tick is the ams ms convention every published AS7341
 * constant uses. Sanity anchor: ATIME 99 / ASTEP 499 -> 139.0 ms. */
#define AMBIT_SPEC_TICK_MS 2.78e-3f
static inline float ambit_spec_tint_ms(uint8_t atime, uint16_t astep)
{
    return ((float)atime + 1.0f) * ((float)astep + 1.0f) * AMBIT_SPEC_TICK_MS;
}

/* Async run state byte returned by AMBIT_CMD_STATUS (must match ambit fw
 * PAM.h AMBIT_ASYNC_*). A measuring ambit doesn't answer at all, so the host
 * infers BUSY from a wake/poll timeout — there is no BUSY byte. */
#define AMBIT_ASYNC_IDLE   0   /* no result buffered (idle, or pre-run race) */
#define AMBIT_ASYNC_DONE   1   /* result buffered, ready to FETCH            */
#define AMBIT_ASYNC_ERROR  2   /* last run failed (alloc/partial)           */

#ifdef __cplusplus
}
#endif

#endif /* AMBYTE_AMBIT_PROTOCOL_H */
