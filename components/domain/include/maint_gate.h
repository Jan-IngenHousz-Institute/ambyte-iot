#ifndef AMBYTE_MAINT_GATE_H
#define AMBYTE_MAINT_GATE_H

/* Admission between the global maintenance lock (self-OTA, AMBIT OTA/flash,
 * schedule update, USB service jobs) and charger register mutations (VIN and
 * IIN limit sets, AICO switch / manual reset / hourly re-arm, charger
 * re-detect pulses). Pure state, no locking: the composition root
 * (main/app_main.c) calls every function inside its maintenance critical
 * section; host tests drive the same functions. docs/charger-aico.md.
 *
 * Why: a "maintenance active?" probe is a snapshot. A mutation that checked it
 * and then waited for the I2C bus could still write the charger after an OTA
 * had begun (Codex review of PR #109, reproduced with the production driver).
 * Here a mutation holds a reservation across its whole sequence, and
 * maintenance cannot begin while any reservation is held:
 *
 *   - mutation_begin is refused (never blocks) while maintenance is active OR
 *     pending; a refused mutation writes nothing and reports
 *     maintenance_active;
 *   - maintenance request with no mutation in flight is granted at once;
 *     with mutations in flight it becomes PENDING: new mutations are refused
 *     from that moment (no starvation), and the requester polls until the
 *     in-flight ones end (granted) or abandons after its own bound (busy).
 *
 * Maintenance WAITS rather than being refused because every charger mutation
 * is short (I2C transactions; the longest is a manual re-detect, ~5 s of
 * settle reads) and a refused OTA would be a failed device in a fleet
 * campaign. The wait cannot deadlock: maintenance requesters run on worker
 * tasks that hold no charger lock (read mutex, I2C bus) while they wait, a
 * mutation never requests maintenance, and a mutation that finds maintenance
 * active or pending is refused instead of waiting.
 *
 * A mutation's admission may outlive the call that took it: the MP2731
 * driver keeps a failing re-detect pulse's admission as a QUARANTINE while
 * BG_EN or USB_DET_EN is not proven back at 0 (mp2731.c, PR #112 review), and
 * releases it once recovery proves both. Maintenance then waits or gives up
 * busy; it is never granted over a dirty charger bit. */

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool     busy;        /* maintenance granted and running */
    bool     pending;     /* maintenance requested, waiting for mutations to end */
    uint32_t mutations;   /* charger mutations in flight */
    uint32_t refused;     /* mutations refused because of maintenance (diagnostic) */
} maint_gate_t;

typedef enum {
    MAINT_GATE_GRANTED = 0,
    MAINT_GATE_WAIT,      /* pending: poll again */
    MAINT_GATE_BUSY,      /* another maintenance op holds or awaits the gate */
} maint_gate_result_t;

static inline bool maint_gate_mutation_begin(maint_gate_t *g)
{
    if (g->busy || g->pending) {
        g->refused++;
        return false;
    }
    g->mutations++;
    return true;
}

static inline void maint_gate_mutation_end(maint_gate_t *g)
{
    if (g->mutations > 0u) g->mutations--;
}

static inline maint_gate_result_t maint_gate_request(maint_gate_t *g)
{
    if (g->busy || g->pending) return MAINT_GATE_BUSY;
    if (g->mutations == 0u) {
        g->busy = true;
        return MAINT_GATE_GRANTED;
    }
    g->pending = true;
    return MAINT_GATE_WAIT;
}

/* Only for the requester that got MAINT_GATE_WAIT. */
static inline maint_gate_result_t maint_gate_poll(maint_gate_t *g)
{
    if (!g->pending) return MAINT_GATE_BUSY;
    if (g->mutations != 0u) return MAINT_GATE_WAIT;
    g->pending = false;
    g->busy = true;
    return MAINT_GATE_GRANTED;
}

/* The requester gave up waiting: mutations are admitted again. */
static inline void maint_gate_abandon(maint_gate_t *g)
{
    g->pending = false;
}

static inline void maint_gate_end(maint_gate_t *g)
{
    g->busy = false;
}

/* Maintenance running or about to: policies hold their automatic actions. */
static inline bool maint_gate_active(const maint_gate_t *g)
{
    return g->busy || g->pending;
}

#endif
