#ifndef AMBYTE_DEVICE_COMMANDS_H
#define AMBYTE_DEVICE_COMMANDS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "esp_err.h"
#include "device_status_port.h"
#include "messaging_port.h"
#include "persistence_port.h"
#include "sensing_port.h"
#include "uart_sensor_port.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    esp_err_t status;
    char message[256];
} cmd_result_t;

typedef struct {
    /* Sensing ports */
    sensor_read_fn              read_env;
    clock_read_fn               read_clock;
    power_read_fn               read_power;   /* MP2731 charger telemetry; NULL = absent */

    /* Persistence ports — one row per measurement event (event-document model) */
    measurement_next_id_fn            next_id;
    measurement_store_event_fn        store_event;
    measurement_claim_next_event_fn   claim_next_event;   /* used by sync_runner */
    measurement_mark_event_synced_fn  mark_event_synced;
    measurement_mark_event_pending_fn mark_event_pending;
    measurement_db_stats_fn           db_stats;            /* read-only event-table stats */

    /* Status port */
    status_set_fn               set_status;

    /* SD-card readiness probe (used by Lua main.lua to gate measurement
     * rounds when the card is out). NULL = SD layer absent. */
    bool                       (*sd_ready)(void);

    /* Messaging ports (Phase 6A) */
    message_publish_fn                  publish;
    message_is_connected_fn             message_is_connected;
    message_set_publish_ack_handler_fn  set_publish_ack_handler;
    message_set_disconnect_handler_fn   set_disconnect_handler;  /* clears in-flight slot on MQTT drop; NULL = only Wi-Fi drop clears it */

    /* Topic config (Phase 6A) */
    const char                         *topic_root;
    const char                         *device_id;

    /* Payload metadata (provisioned via BLE / NVS pre-pop) */
    const char                         *protocol_id;
    const char                         *device_name;
    const char                         *device_version;
    const char                         *device_firmware;
    const char                         *timezone;      /* IANA name, "" = unset */

    /* UART sensor ports (Phase 7) */
    uart_sensor_query_fn                uart_query;        /* AMBIT binary */
    uart_sensor_ping_fn                 uart_ping;
    uart_sensor_status_fn               uart_status;
    uart_sensor_text_query_fn           uart_text_query;   /* generic ASCII line */
    uart_sensor_stream_query_fn         uart_stream_query; /* multi-line until sentinel */

    /* Optional heap hook: request a garbage collection in the scripting VM (wired
     * to lua_runner_request_gc). Before a large MQTT publish the publisher asks for
     * a GC to de-fragment the shared heap; NULL = no GC available (the publish
     * still heap-gates and defers if the largest free block is too small). */
    void                              (*request_gc)(void);
} device_commands_config_t;

esp_err_t device_commands_init(const device_commands_config_t *cfg);

/* This device's Wi-Fi STA MAC as "AA:BB:CC:DD:EE:FF" (read once at init). Returns
 * an empty string if the MAC was unavailable when device_commands_init ran. */
const char *device_commands_get_mac(void);

/* Measurement-activity gate. Bracket any latency-sensitive sensor transaction
 * with begin()/end(); the background sync runner pauses publishing while the
 * count is non-zero and drains once it returns to idle. Safe to nest. */
void device_commands_measurement_begin(void);
void device_commands_measurement_end(void);
bool device_commands_measurement_active(void);

/* Register a callback fired whenever a measurement event is stored, and when a
 * measurement burst finishes — the sync_runner registers its task-notify here so
 * it can wake and drain on demand instead of polling. NULL clears it. The hook
 * runs in the caller's (storing) task context; keep it cheap (a task notify). */
void device_commands_set_sync_notifier(void (*fn)(void));

/* Phase-1 power gate: true when it's OK to drain the MQTT backlog — i.e. the
 * device is on external (solar/USB) power, so battery-only operation doesn't
 * spend the radio's energy budget. Keyed on VIN-present (not input current) and
 * debounced with on/off dwell times to avoid flapping near the threshold. The
 * charger reading is cached so the 10 s sync cycle and the back-to-back drain
 * loop don't each pay an ADC conversion. Returns true (never gates) when no
 * power monitor is wired in, preserving behaviour on boards without the charger. */
bool device_commands_publish_power_ok(void);

cmd_result_t cmd_set_rgb(uint8_t r, uint8_t g, uint8_t b);
cmd_result_t cmd_read_rtc(time_t *out_time);
cmd_result_t cmd_device_status(bool *bme_ready, bool *rtc_ready, time_t *rtc_time);
cmd_result_t cmd_read_env(float *temp, float *hum, float *pres);

/* Read on-board power telemetry (battery / input / system voltage, charge /
 * input current) from the MP2731 charger. Pass NULL to ignore the values; the
 * result message carries a human-readable summary. ESP_ERR_NOT_SUPPORTED when
 * no power monitor is wired in. */
cmd_result_t cmd_read_power(power_reading_t *out);

/* Drive a PWM signal on GPIO4 via LEDC. duty_pct is 0..100 (float precision);
 * freq_hz is the PWM frequency in Hz (e.g. 10000). When enable is false the
 * output is stopped and the pin held low (duty/freq ignored). The duty
 * resolution is chosen automatically from freq_hz (up to 14-bit). */
cmd_result_t cmd_pwm(float duty_pct, uint32_t freq_hz, bool enable);

/* Read BME280 and persist temperature/humidity/pressure as one event
 * (payload {"temperature":..,"humidity":..,"pressure":..}, cmd_raw
 * "device.bme280"). The background sync task (sync_runner) publishes it as one
 * MQTT message. Either out-pointer may be NULL; out_reading receives the
 * measured values (this is the fused-store backing of Lua device.bme280). */
cmd_result_t cmd_record_env(int64_t *out_measure_id, measurement_t *out_reading);

/* Returns true when the SD card is mounted (and therefore the SQLite event DB
 * is writable). main.lua should consult this before starting a measurement
 * round so it doesn't measure into a closed DB. */
cmd_result_t cmd_sd_ready(bool *out_ready);
cmd_result_t cmd_log(const char *msg);
cmd_result_t cmd_sleep_ms(uint32_t ms);

/* Point-in-time device status (Wi-Fi + provisioning, event DB, MP2731 power and
 * publish-gate state) — the same facts the `status` CLI prints. Gathered by
 * cmd_status_report() for the Lua heartbeat, which stores it as a sensor="status"
 * event so the sole publisher (sync_runner) forwards it under the power gate. */
typedef struct {
    bool            wifi_connected;
    bool            provisioned;
    bool            db_online;
    bool            power_valid;        /* false if no charger / read failed */
    power_reading_t power;              /* valid only when power_valid */
    bool            publish_gate_open;  /* device_commands_publish_power_ok() */
    bool            env_valid;          /* false if no BME280 / read failed */
    float           temperature_c;      /* valid only when env_valid */
    float           humidity_percent;   /* valid only when env_valid */
    float           pressure_pa;        /* valid only when env_valid */
} device_status_snapshot_t;

cmd_result_t cmd_status_report(device_status_snapshot_t *out);

/* Store one STATUS heartbeat event (tag STATUS, onboard provenance) built from
 * cmd_status_report. Called by sync_runner on its heartbeat period — status
 * reporting is firmware-owned so a broken/missing main.lua can't silence it.
 * Does not wake the sync runner (the caller is the sync runner). */
cmd_result_t cmd_store_status_event(void);

/* Last battery voltage (mV) latched from any successful charger read;
 * 0 = never read. Cheap probe (no I2C) for the status-LED blinker. */
uint32_t device_commands_last_battery_mv(void);

/* Store one measurement event from a v2 descriptor (see persistence_port.h).
 * desc->payload_json and desc->tag are required; channel/device/cmd_raw/
 * metadata_json may be NULL/"". Requires the SD-backed event log. */
cmd_result_t cmd_store_event(const measurement_event_desc_t *desc);

/* Send an ASCII command and read multiple response lines until one contains
 * `sentinel` (or timeout). Pre-wakes the port. Used for the AMBIT PLOTTING run.
 * `out` is NUL-terminated; *out_len excludes the NUL. */
cmd_result_t cmd_uart_stream_query(uint8_t channel, const char *cmd,
                                   const char *sentinel, uint32_t timeout_ms,
                                   char *out, size_t out_cap, size_t *out_len);
cmd_result_t cmd_next_measure_id(int64_t *out_id);

/* MQTT commands (Phase 6A) */
/* Publish the next pending event as one MQTT message (one measure_id = one
 * message; used by the sync_runner). */
cmd_result_t cmd_mqtt_publish_next_event(void);
cmd_result_t cmd_mqtt_status(void);

/* Event-DB / sync-backlog stats. *available = DB online (SD mounted), *total
 * rows, *pending = rows not yet synced (sync_state != SYNCED), *next_id = next
 * measure_id. Any out-pointer may be NULL. */
cmd_result_t cmd_db_status(bool *available, int64_t *total,
                           int64_t *pending, int64_t *next_id);

/* UART sensor commands — raw (Phase 7) */
cmd_result_t cmd_uart_query(uint8_t channel, const uint8_t cmd[8],
                            const uint8_t *extra, size_t extra_len,
                            size_t expect_raw,
                            uart_sensor_response_t *response,
                            uint32_t timeout_ms);
cmd_result_t cmd_uart_ping(uint8_t channel, bool *connected);
cmd_result_t cmd_uart_status(void);

/* Generic ASCII line-oriented UART query (transport/diagnostic — NEVER stores;
 * schema-v2 rule: measurement commands store, transport commands don't).
 *
 * Sends `cmd` followed by `terminator` over UART channel `channel`, then
 * reads one line (until `terminator` again) into `out_resp` or aborts after
 * `timeout_ms`. If the first line echoes the sent command verbatim it is
 * discarded and the next line is returned. `out_resp` is always set
 * (NUL-terminated, possibly empty) on return.
 */
cmd_result_t cmd_uart_text_query(uint8_t channel,
                                 const char *cmd, const char *terminator,
                                 uint32_t timeout_ms,
                                 char *out_resp, size_t resp_cap,
                                 size_t *resp_len);

/* Ambit sensor commands — typed wrappers (Phase 7) */

/* Configuration (ack-only — no CMD_END) */
cmd_result_t cmd_ambit_set_gains(uint8_t ch, uint8_t fluo, uint8_t fluoref,
                                  uint8_t ir, uint8_t irref,
                                  uint8_t sun, uint8_t leaf);
cmd_result_t cmd_ambit_set_currents(uint8_t ch, uint8_t i620, uint8_t i720,
                                     uint8_t ir);
cmd_result_t cmd_ambit_config_detector(uint8_t ch);

/* Queries (immediate raw response) */
cmd_result_t cmd_ambit_get_temp(uint8_t ch, float *leaf_temp, float *chip_temp);
cmd_result_t cmd_ambit_get_spec(uint8_t ch, uint16_t spec[10], float *par);
cmd_result_t cmd_ambit_get_temp_raw(uint8_t ch, float *leaf, float *leaf1,
                                     float *chip, int16_t raw[4]);
cmd_result_t cmd_ambit_get_info(uint8_t ch, uint8_t info_type,
                                 uint8_t *out, size_t out_size, size_t *out_len);

/* Cached per-channel AMBIT identity + config for event provenance.
 *
 * Two halves with different lifecycles:
 *  - identity/calibration (valid, device_id, fw_version, cal_version, ambit_name,
 *    actinic_coef): STATIC per connection. Fetched once via cmd 33 (FW + calib)
 *    and cached; a measurement reads them with zero UART cost. On the first
 *    successful fetch after a (re)connect the firmware emits one DEVICE_INFO
 *    event carrying the full calibration.
 *  - gains/currents: MUTABLE, but only the ambyte changes them (cmd 1/cmd 2;
 *    the AMBIT has no read-back). Tracked at set-time, so they're known without
 *    a query. Reset to "unset" on (re)connect (the AMBIT booted with its own
 *    defaults, unknown to us, until a set_gains/set_currents). */
typedef struct {
    bool     valid;
    char     device_id[18];   /* "AA:BB:CC:DD:EE:FF" from the AMBIT efuse MAC */
    char     fw_version[16];   /* "major.minor.batch" */
    uint32_t cal_version;      /* CRC32 of the calibration struct (bumps on any cal change) */
    char     ambit_name[20];   /* calibration ambit_name, e.g. "AmbitV003" */
    float    actinic_coef;     /* PAR(µmol)→DAC byte = actinic_coef × PAR; 0 if cal unread */
    bool     gains_set;        /* true once the ambyte has set gains this connection */
    uint8_t  gains[6];         /* fluo, fluoref, ir, irref, sun, leaf (cmd 1 order) */
    bool     currents_set;     /* true once the ambyte has set currents this connection */
    uint8_t  currents[3];      /* i620, i720, ir (cmd 2 order) */
} ambit_device_info_t;

/* Return cached identity+config for `ch`, lazily fetching identity on first use
 * (one-time UART cost, then free). *out is zeroed + valid=false on a fetch
 * failure (gains/currents fields still reflect any tracked set commands). */
cmd_result_t cmd_ambit_device_info(uint8_t ch, ambit_device_info_t *out);
/* Drop a channel's identity cache AND tracked gains/currents so the next read
 * re-fetches and re-announces — call on (re)connect / swap. */
void cmd_ambit_device_info_invalidate(uint8_t ch);

/* Measurements (FSM response) */
cmd_result_t cmd_ambit_run(uint8_t ch, const uint8_t *run_arr, uint8_t arr_len,
                            uint8_t led_persist, bool allow_interrupt,
                            uart_sensor_response_t *response, uint32_t timeout_ms);
cmd_result_t cmd_ambit_run_mpf(uint8_t ch, uint16_t length, uint8_t interval,
                                bool change_act, uint8_t act,
                                uart_sensor_response_t *response, uint32_t timeout_ms);

/* Parallel measurement protocol (trigger → poll → fetch). cmd_ambit_trigger
 * (cmd 22) starts a retained run and returns on ack; cmd_ambit_poll (cmd 23)
 * reads the async state byte (AMBIT_ASYNC_* in ambit_protocol.h), mapping a
 * timeout to "busy"; cmd_ambit_fetch (cmd 24) streams the buffered arrays,
 * yielding the same `response` as cmd_ambit_run. */
cmd_result_t cmd_ambit_trigger(uint8_t ch, const uint8_t *run_arr, uint8_t arr_len,
                                uint8_t led_persist, bool allow_interrupt,
                                uint32_t timeout_ms);
cmd_result_t cmd_ambit_poll(uint8_t ch, uint8_t *state, uint32_t timeout_ms);
cmd_result_t cmd_ambit_fetch(uint8_t ch, uart_sensor_response_t *response,
                                uint32_t timeout_ms);

/* Actions (wait for CMD_END, no response data) */
cmd_result_t cmd_ambit_blink(uint8_t ch, uint8_t ambit_id, uint8_t intensity);
cmd_result_t cmd_ambit_calibrate_baseline(uint8_t ch);
cmd_result_t cmd_ambit_actinic(uint8_t ch, uint8_t type, uint8_t var, uint8_t var2);

/* Write commands (extra data buffered, wait for CMD_END) */
cmd_result_t cmd_ambit_set_metadata(uint8_t ch, const uint8_t *metadata, size_t len);

/* AMBIT OTA-over-UART (cmds 25-28). Stream a new C3 firmware image: begin(size),
 * then data(seq, chunk) for each <=AMBIT_OTA_CHUNK_MAX-byte chunk in order, then
 * end() (the AMBIT verifies the image + reboots into the new slot). *status (may
 * be NULL) receives the AMBIT's 1-byte reply — 0 = ok; non-zero = a per-command
 * error code (OTA_DATA: 1 CRC, 2 out-of-order, 3 write-fail, 4 bad-len, 5 short,
 * 6 no-begin). The CRC16 over each chunk is computed here. Orchestrated by
 * components/ambit_ota; not for use from main.lua. */
cmd_result_t cmd_ambit_ota_begin(uint8_t ch, uint32_t image_size, uint8_t *status);
cmd_result_t cmd_ambit_ota_data(uint8_t ch, uint16_t seq,
                                const uint8_t *chunk, uint8_t len, uint8_t *status);
cmd_result_t cmd_ambit_ota_end(uint8_t ch, uint8_t *status);
cmd_result_t cmd_ambit_ota_abort(uint8_t ch, uint8_t *status);
/* Mark the AMBIT's just-booted image valid (cancel its pending rollback). Sent
 * after the post-OTA reboot once the new image is seen to answer; if never sent,
 * the AMBIT bootloader rolls back to the previous image on its next reboot. */
cmd_result_t cmd_ambit_ota_confirm(uint8_t ch, uint8_t *status);

/* Clear any in-flight publish slot (revert it to PENDING). Called from both the
 * Wi-Fi disconnect handler and, via set_disconnect_handler, the MQTT-level
 * disconnect — so a broker/TLS drop that leaves Wi-Fi associated still frees the
 * slot instead of wedging the drain until reboot. */
void device_commands_on_mqtt_disconnect(void);

/* Clear the in-flight publish slot WITHOUT marking the event pending — the caller
 * has already rewound the persistence cursor (see event_log_rewind), so the event
 * is pending by position. Frees the slot so the drain can re-publish immediately
 * instead of waiting out the reaper. A late PUBACK for the abandoned msg_id is a
 * no-op. Used by the `evlog rewind` CLI command. */
void device_commands_abort_inflight(void);

/* Revert a stale in-flight publish slot to PENDING if it has been latched longer
 * than max_age_ms. A lost/expired PUBACK (e.g. esp-mqtt outbox expiry) with the
 * connection nominally up leaves no callback, so without this the one-in-flight
 * slot never clears and the whole drain wedges until reboot. Returns true if a
 * stale slot was reaped. Called by the sync_runner while draining. */
bool device_commands_reap_stale_inflight(int64_t max_age_ms);

/* Diagnostic: report the in-flight publish slot. *msg_id and *measure_id are -1
 * when idle; *age_ms is ms since the slot was latched (0 when idle). Any
 * out-pointer may be NULL. */
void device_commands_inflight_status(int *msg_id, int64_t *measure_id, int64_t *age_ms);

/* Test hook: inject a fake, already-stale in-flight slot (no real measure_id) so
 * the reaper path can be exercised on hardware without engineering a lost PUBACK.
 * The next sync_runner drain (kick it with sync_runner_notify) reaps it. */
void device_commands_inject_stale_inflight(void);

/* Milliseconds since the last successful PUBACK (monotonic; seeded to boot time).
 * The connectivity watchdog uses this to detect a device that should be
 * publishing but cannot. Returns 0 if the reading couldn't be taken (treated as
 * healthy so the watchdog never reboots on transient contention). */
int64_t device_commands_ms_since_publish_ok(void);

#ifdef __cplusplus
}
#endif

#endif
