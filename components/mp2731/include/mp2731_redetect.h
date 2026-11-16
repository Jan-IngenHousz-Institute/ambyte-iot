#ifndef AMBYTE_MP2731_REDETECT_H
#define AMBYTE_MP2731_REDETECT_H

/* Automatic charger re-detection: WHEN the firmware pulses the MP2731's BG_EN
 * bit, as a pure state machine over the existing power reads. No I2C, no
 * clock, no logging: mp2731.c feeds every read through here and performs the
 * register writes; tests/charger_redetect_policy_host.c drives this file
 * verbatim. See docs/charger-redetect.md.
 *
 * Field origin (Uluru, 15-17 Nov 2026, fw 2.21.0): the charger sits idle in
 * full sun for hours. 14:63:93:39:36:F8 on 17 Nov: REG0C = 0x00 ("no input",
 * not charging) with an unloaded 7.4-7.6 V on VIN and VSYS 3.5 V from ~07:22
 * on, no fault except the expired I2C watchdog (normal on this fleet), safety
 * timer not expired, HIZ off, charge enabled. Input qualification needs only
 * VIN > VBATT + 0.3 V and UVLO < VIN < OVP (datasheet Rev 1.2, "Device
 * Power-Up from Input Source"), so the chip had every reason to start and
 * did not. A gateway reboot never restarted it (0/3; the ESP reset does not
 * reset the charger); a brownout - a charger power cycle - did (3/4).
 * Datasheet REG09 bit 3 BG_EN: "Setting this bit from 1 to 0 will reset the
 * input plug-in detection and safety timer." That is the software equivalent
 * of re-inserting the input, without a power cycle.
 *
 * The decision, all of these continuously for CRD_DWELL_MS:
 *   - a valid read (ADC frame accepted, live status bytes complete);
 *   - VIN_STAT 000 AND CHG_STAT 00: the chip says no input and is not
 *     charging. Never while charging, never on 11 (done), and never in a
 *     qualified-but-idle VIN_STAT (a state nobody has shown BG_EN fixes);
 *   - no fault except WATCHDOG (no input OVP / "no input" INPUT_FAULT, no
 *     thermal shutdown, no battery OVP, no NTC fault, no OTG fault), no NTC
 *     float, no thermal regulation;
 *   - VIN >= CRD_VIN_FLOOR_MV and VIN >= VSYS + CRD_VIN_OVER_VSYS_MV;
 *   - VSYS < CRD_VSYS_FULL_MV (a full battery is left alone);
 *   - the ADC frame changed at least once in the dwell (a frozen register
 *     file cannot keep re-arming retries).
 * Then, at the moment of firing, the glue reads REG00/01/04/17 and
 * crd_precheck() refuses HIZ, charge disabled, an EXPIRED SAFETY TIMER (the
 * pulse would clear that fault: never auto-defeat a protection, only report
 * it), and VIN < VIN_MIN + CRD_VIN_OVER_LIMIT_MV.
 *
 * Retry: first after the dwell, then not before 10, 20, 40, 60, 60 ... min
 * after the previous attempt (and always after a fresh full dwell); at most
 * CRD_MAX_ATTEMPTS_PER_BOOT per boot. Only CRD_SUSTAIN_MS of continuous
 * charging resets the backoff: a panel that charges for a few seconds and
 * collapses again (07:00 on 17 Nov) must not re-arm a 3-minute retry loop. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Idle evidence needed before a pulse. The chip's own transitions finish in
 * seconds (DCD timer 500 ms + 50 ms adapter timer + primary/secondary
 * detection, then buck soft-start), and the 17 Nov dawn bursts restarted on
 * their own within tens of seconds; 3 min of a stable "no input" with a high
 * VIN excludes those and a passing cloud, while the stuck state lasted hours. */
#define CRD_DWELL_MS            (3 * 60 * 1000)

/* Readers sample every 5-15 s (publish gate 5 s cache, SD guard 15 s). A
 * longer silence is not continuous evidence: restart the dwell / sustain. */
#define CRD_MAX_GAP_MS          45000

/* Absolute VIN floor. Above any USB/5 V adapter (USB allows at most 5.25 V),
 * so a unit on a 5 V source can never trigger even if it stopped charging. A
 * 6 V-class panel (Voc ~8.2 V in sun) unloaded below 5.5 V is in deep
 * twilight: worth nothing to charge from. */
#define CRD_VIN_FLOOR_MV        5500u

/* VIN over VSYS. Qualification needs VIN > VBATT + VHDRM (0.3 V rising). The
 * battery ADC reads 0 while the charger is idle (field and bench), so VSYS is
 * the battery proxy: with no qualified input the system runs from the battery
 * through the BATFET (supplement mode), VSYS = VBATT - I*Rds(on), i.e. at or
 * a few tens of mV BELOW the battery. 1.0 V covers that drop, the 60 mV VIN /
 * 20 mV VSYS ADC LSBs, and leaves ~0.7 V above the chip's own headroom rule:
 * a VIN that clears it is unambiguously a source the chip should accept.
 * With today's floor and full gate it never binds (5.5 V - 4.1 V = 1.4 V);
 * it is kept so that lowering the floor can never drop the headroom rule. */
#define CRD_VIN_OVER_VSYS_MV    1000u

/* VIN over the programmed VIN_MIN (REG01). Charging regulates the input down
 * to VIN_MIN (VINPPM, REG14 bit 7 seen at 4.98 V on 17 Nov with a 5.0 V
 * limit); an unloaded VIN only just above it is a panel with almost no power
 * to give. Read at firing time, so a field VIN-limit experiment is honoured. */
#define CRD_VIN_OVER_LIMIT_MV   500u

/* VSYS at/above this = battery (nearly) full: leave it alone. VBATT_REG is
 * 4.2 V (OTP default); idle VSYS tracks the battery, so 4.1 V is ~90 %. */
#define CRD_VSYS_FULL_MV        4100u

/* After a pulse: how long to look for the outcome. Detection settles in
 * ~1 s; 60 s leaves room for the buck to start and CHG_STAT to report. */
#define CRD_OUTCOME_WINDOW_MS   60000

/* Backoff after an attempt that did not lead to sustained charging. */
#define CRD_BACKOFF_FIRST_MS    (10 * 60 * 1000)
#define CRD_BACKOFF_MAX_MS      (60 * 60 * 1000)
/* A blocked precheck (e.g. safety timer expired) is re-checked this often:
 * re-reading four registers is cheap, but no faster than the first backoff. */
#define CRD_BLOCKED_RECHECK_MS  CRD_BACKOFF_FIRST_MS

/* Continuous charging that proves the panel holds: resets the backoff. */
#define CRD_SUSTAIN_MS          (10 * 60 * 1000)

/* Per boot. Units reboot nightly (01:20 UTC = 10:50 ACST, mid-day at Uluru);
 * 24 attempts at a 60 min cap is a full day of daylight. */
#define CRD_MAX_ATTEMPTS_PER_BOOT 24u

/* Fault bits that block (REG0D): everything except WATCHDOG_FAULT (bit 7),
 * which is set on every unit of this fleet (the host never kicks it). */
#define CRD_FAULT_BLOCK_MASK    0x7Fu
/* Status bits that block (REG0C): NTC float (bit 2), thermal regulation (bit 1). */
#define CRD_STATUS_BLOCK_MASK   0x06u

typedef enum {
    CRD_OUT_NONE = 0,     /* no attempt yet */
    CRD_OUT_PENDING,      /* pulse done, outcome window open */
    CRD_OUT_CHARGING,     /* CHG_STAT reached 01/10 inside the window */
    CRD_OUT_DETECTED,     /* VIN_STAT left 000, but no charging seen */
    CRD_OUT_NO_CHANGE,    /* still "no input" and idle when the window closed */
    CRD_OUT_BUS_ERROR,    /* I2C failed before/while pulsing */
    CRD_OUT_NOT_LATCHED,  /* BG_EN=1 never read back: no 1->0 edge happened */
    CRD_OUT_NOT_CLEARED,  /* BG_EN stuck at 1 after retries: auto disabled */
} crd_outcome_t;

typedef enum {
    CRD_BLOCK_NONE = 0,
    CRD_BLOCK_HIZ,              /* REG00 bit 7 */
    CRD_BLOCK_CHARGE_DISABLED,  /* REG04[5:4] != 01 */
    CRD_BLOCK_SAFETY_TIMER,     /* REG17 bit 7: report, never auto-clear */
    CRD_BLOCK_VIN_LIMIT,        /* VIN < VIN_MIN + margin */
    CRD_BLOCK_BUS,              /* precheck registers unreadable */
} crd_block_t;

typedef enum {
    CRD_ENABLED = 0,
    CRD_DISABLED_OPERATOR,      /* CLI/MQTT switch, RAM only */
    CRD_DISABLED_RESET,         /* the unit reset while an attempt was open */
    CRD_DISABLED_BG_EN_STUCK,   /* BG_EN could not be cleared */
} crd_disabled_t;

/* Result of the register pulse, as reported back by the glue. */
typedef enum {
    CRD_PULSE_OK = 0,           /* 1 latched, 0 read back */
    CRD_PULSE_BUS_ERROR,
    CRD_PULSE_NOT_LATCHED,
    CRD_PULSE_NOT_CLEARED,
} crd_pulse_t;

typedef enum {
    CRD_NONE = 0,
    CRD_CHECK,      /* dwell met, allowed: glue reads the precheck registers */
    CRD_RESOLVED,   /* an attempt's outcome was decided by this observation */
} crd_action_t;

/* One power read, as the policy needs it. */
typedef struct {
    int64_t  now_ms;        /* monotonic */
    bool     status_valid;  /* REG0C/REG0D completed (also for rejected frames) */
    bool     adc_ok;        /* the reader accepted the frame (ESP_OK) */
    bool     adc_changed;   /* ADC frame differs from the previous accepted one */
    uint8_t  status, fault; /* REG0C, REG0D */
    uint16_t vin_mv, vsys_mv;
} crd_obs_t;

typedef struct {
    crd_disabled_t disabled;
    bool     have_obs;
    int64_t  last_obs_ms;
    /* dwell */
    bool     dwelling, dwell_changed, veto_counted;
    int64_t  dwell_start_ms;
    /* retry pacing */
    bool     has_next;
    int64_t  next_allowed_ms;
    uint32_t backoff_ms;       /* last applied backoff; 0 = next one is the first */
    /* open attempt */
    bool     pending;
    int64_t  attempt_ms;
    bool     saw_detect, saw_charge;
    uint8_t  last_status_in_window;
    /* sustained charging */
    bool     charging_run;
    int64_t  charging_since_ms;
    /* counters (per boot) */
    uint32_t attempts, charging, detected, no_change, failed, blocked, vetoed, manual;
    crd_outcome_t last_outcome;
    crd_block_t   last_block;
    int64_t  last_attempt_ms;
    uint8_t  last_before, last_after;  /* REG0C at the pulse / at resolution */
    uint16_t last_vin_mv, last_vsys_mv;
} crd_t;

void crd_init(crd_t *s);

/* True when a single observation meets every live-status/voltage condition. */
bool crd_eligible(const crd_obs_t *o);

/* Feed one read. `veto` = maintenance lock (OTA / AMBIT flash) right now: a
 * met dwell waits for it to clear (counted once per wait in `vetoed`). */
crd_action_t crd_step(crd_t *s, const crd_obs_t *o, bool veto);

/* Precheck registers read at firing time. */
crd_block_t crd_precheck(uint8_t reg00, uint8_t reg01, uint8_t reg04, uint8_t reg17,
                         uint16_t vin_mv);

/* After CRD_CHECK: the precheck refused (not an attempt). */
void crd_note_blocked(crd_t *s, int64_t now_ms, crd_block_t why);
/* After CRD_CHECK + precheck OK: the pulse result. `status_before` = REG0C. */
void crd_note_attempt(crd_t *s, int64_t now_ms, crd_pulse_t result, uint8_t status_before,
                      uint16_t vin_mv, uint16_t vsys_mv);
/* A manual pulse (CLI/MQTT): counted, restarts the dwell, never an auto attempt. */
void crd_note_manual(crd_t *s);

void crd_set_disabled(crd_t *s, crd_disabled_t why);

/* Stable telemetry spellings (health.power.charger.redetect). Never rename. */
const char *crd_outcome_name(crd_outcome_t o);
const char *crd_block_name(crd_block_t b);
const char *crd_disabled_name(crd_disabled_t d);
/* "disabled" | "capped" | "pending" | "backoff" | "dwell" | "idle" */
const char *crd_state_name(const crd_t *s, int64_t now_ms);

/* JSON object for health.power.charger.redetect. Returns length or -1.
 * CRD_RENDER_CAP bounds the worst case (all counters at UINT32_MAX, int64
 * timestamps): the charger object's 2048 B cap keeps ~700 B of headroom. */
#define CRD_RENDER_CAP 512u
int crd_render(const crd_t *s, int64_t now_ms, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
