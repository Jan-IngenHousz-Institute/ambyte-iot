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
#define AMBIT_RESP_TEMP_SIZE       4   /* 2 × int16_t   (leaf*10, chip*10) */
#define AMBIT_RESP_TEMP_RAW_SIZE  14   /* 7 × int16_t */
#define AMBIT_RESP_STATUS_SIZE     1   /* 1 × uint8_t   (async run state)   */

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
