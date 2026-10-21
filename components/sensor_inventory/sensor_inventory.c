#include "sensor_inventory.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

static sensor_inventory_ports_t io;
static sensor_snapshot_t cache;
static bool initialized, refreshing, recovering, recovery_transmitted, legacy_reading;
static unsigned vetoes;
static int64_t next_block_ms, idle_ms, idle_anchor_ms;
static struct {
    int64_t due_ms, spec_due_ms;
    unsigned phase;
    uint32_t epoch;
    bool no_spec, announce_pending;
    uint32_t announced_crc;
    char announced_id[18], announced_fw[16];
} work[SENSOR_PORTS];
static struct run_state {
    uint64_t epoch;
    int64_t window_end_ms, due_ms;
    unsigned attempts;
    uint32_t refusal_ms;
} runs[SENSOR_PORTS];

static void lock(void) { if (io.lock) io.lock(); }
static void unlock(void) { if (io.unlock) io.unlock(); }
static int64_t now_ms(void) { return io.now_us ? io.now_us() / 1000 : 0; }
static int64_t bank_until_locked(void)
{
    int64_t until = 0;
    for (unsigned ch = 0; ch < SENSOR_PORTS; ++ch)
        if ((cache.busy & (1u << ch)) && runs[ch].window_end_ms > until)
            until = runs[ch].window_end_ms;
    return until;
}
static bool blocked_locked(uint8_t ch)
{ return (cache.busy & (1u << ch)) || now_ms() < bank_until_locked(); }
/* Retry/revalidation budgets accumulate only while the bank is available;
 * a ten-minute acquisition cannot consume ten minutes of inspection idle time. */
static int64_t idle_now_locked(void)
{
    int64_t real = now_ms();
    int64_t anchor = bank_until_locked();
    if (anchor < idle_anchor_ms) anchor = idle_anchor_ms;
    if (!vetoes && real > anchor) idle_ms += real - anchor;
    idle_anchor_ms = real;
    return idle_ms;
}
static void increment(uint32_t *n) { if (*n != UINT32_MAX) ++*n; }
static void maximum(uint32_t *n, uint32_t v) { if (*n < v) *n = v; }
static uint32_t elapsed_us(int64_t start)
{
    int64_t value = io.now_us ? io.now_us() - start : 0;
    return value < 0 ? 0 : value > UINT32_MAX ? UINT32_MAX : (uint32_t)value;
}
void sensor_inventory_wake(void) { if (io.wake) io.wake(); }
bool sensor_inventory_ready(void) { return initialized; }
void sensor_inventory_init(const sensor_inventory_ports_t *ports, uint64_t boot_id)
{
    io = *ports;
    memset(&cache, 0, sizeof cache);
    memset(work, 0, sizeof work);
    memset(runs, 0, sizeof runs);
    cache.boot_id = boot_id;
    next_block_ms = 0; refreshing = recovering = false; legacy_reading = false; vetoes = 0;
    idle_ms = 0; idle_anchor_ms = now_ms();
    initialized = boot_id != 0 && io.now_us && io.lock && io.unlock;
    sensor_inventory_wake();
}
static sensor_stamp_t stamp(void)
{
    sensor_stamp_t s = {.uptime_ms = now_ms(), .valid = true};
    s.utc_ms = io.utc_ms ? io.utc_ms() : 0;
    if (s.utc_ms < 1704067200000LL) s.utc_ms = 0;
    s.clock = io.clock ? io.clock(&s.suspect) : 0;
    return s;
}
uint32_t sensor_inventory_crc32(const void *data, size_t size)
{
    const unsigned char *p = data;
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < size; ++i) {
        crc ^= p[i];
        for (unsigned b = 0; b < 8; ++b)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}
static void invalidate_locked(uint8_t ch)
{
    uint32_t generation = cache.ports[ch].generation;
    memset(&cache.ports[ch], 0, sizeof cache.ports[ch]);
    cache.ports[ch].generation = generation;
    work[ch].phase = 0; work[ch].due_ms = idle_now_locked();
    ++work[ch].epoch;
    work[ch].spec_due_ms = 0; work[ch].no_spec = false;
    work[ch].announce_pending = false;
}
void sensor_inventory_invalidate(uint8_t ch)
{
    if (ch >= SENSOR_PORTS) return;
    lock(); invalidate_locked(ch); unlock(); sensor_inventory_wake();
}
void sensor_inventory_reset(void)
{
    lock();
    for (uint8_t ch = 0; ch < SENSOR_PORTS; ++ch) invalidate_locked(ch);
    (void)idle_now_locked(); cache.busy = cache.stalled = 0;
    for (unsigned ch = 0; ch < SENSOR_PORTS; ++ch) {
        if (runs[ch].epoch != UINT64_MAX) ++runs[ch].epoch;
        runs[ch].due_ms = runs[ch].window_end_ms = INT64_MAX;
    }
    /* The reset owner's veto remains held through application boot grace. */
    unlock(); sensor_inventory_wake();
}
bool sensor_inventory_snapshot(sensor_snapshot_t *out)
{
    if (!out) return false;
    lock(); *out = cache; out->captured_uptime_ms = now_ms(); unlock();
    return initialized;
}
bool sensor_inventory_cached(uint8_t ch, ambit_device_info_t *out)
{
    if (ch >= SENSOR_PORTS || !out) return false;
    lock(); *out = cache.ports[ch].identity; unlock();
    return out->valid;
}
void sensor_inventory_config(uint8_t ch, const uint8_t *gains, const uint8_t *currents)
{
    if (ch >= SENSOR_PORTS) return;
    lock();
    ambit_device_info_t *e = &cache.ports[ch].identity;
    if (gains) { memcpy(e->gains, gains, 6); e->gains_set = true; }
    if (currents) { memcpy(e->currents, currents, 3); e->currents_set = true; }
    unlock();
}
static uint32_t u32(const unsigned char *p)
{ return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static float f32(const unsigned char *p)
{ uint32_t bits = u32(p); float v; memcpy(&v, &bits, 4); return v; }

static bool accept_locked(uint8_t ch, uint8_t type, const unsigned char *b,
                           size_t size, sensor_stamp_t observed)
{
    sensor_port_snapshot_t *p = &cache.ports[ch];
    ambit_device_info_t *e = &p->identity;
    if (type == 2) {
        if (size != 48) return false;
        bool zero = true, ff = true;
        for (unsigned i = 8; i < 14; ++i) { zero &= b[i] == 0; ff &= b[i] == 255; }
        if (zero || ff) return false;
        char id[18];
        snprintf(id, sizeof id, "%02X:%02X:%02X:%02X:%02X:%02X", b[8], b[9], b[10], b[11], b[12], b[13]);
        if (e->valid && strcmp(e->device_id, id)) {
            invalidate_locked(ch); p = &cache.ports[ch]; e = &p->identity;
        }
        if (!e->valid) {
            if (p->generation == UINT32_MAX) return false;
            ++p->generation;
        }
        memcpy(e->device_id, id, sizeof id);
        snprintf(e->fw_version, sizeof e->fw_version, "%u.%u.%u", b[0], b[1], b[2]);
        e->hw_rev = b[28]; e->valid = true;
        p->seen = p->present = true; p->revalidation_failed = false;
        p->presence_stamp = p->fw_stamp = observed;
        return true;
    }
    if (!e->valid) return false;
    if (type == 1) {
        if (size != SENSOR_CAL_BYTES) return false;
        memcpy(p->calibration, b, size);
        p->have_cal = true; p->cal_stamp = observed;
        e->cal_version = sensor_inventory_crc32(b, size);
        memcpy(e->ambit_name, b, sizeof e->ambit_name - 1);
        e->ambit_name[sizeof e->ambit_name - 1] = 0;
        e->actinic_coef = f32(b + 108); e->tick_factor = f32(b + 136);
        work[ch].announce_pending = e->cal_version != work[ch].announced_crc ||
            strcmp(e->device_id, work[ch].announced_id) || strcmp(e->fw_version, work[ch].announced_fw);
        return true;
    }
    if (type == 4 && size == SENSOR_SPEC_BYTES && b[0] == 1 &&
            b[1] == 0 && b[2] == 0 && b[3] == 0) {
        memcpy(p->spectral, b, size); p->have_spec = true; p->spec_stamp = observed;
        return true;
    }
    return false;
}
static void announce(uint8_t ch)
{
    ambit_device_info_t identity;
    unsigned char cal[SENSOR_CAL_BYTES];
    uint32_t generation;
    lock();
    bool pending = work[ch].announce_pending && cache.ports[ch].have_cal;
    identity = cache.ports[ch].identity; generation = cache.ports[ch].generation;
    memcpy(cal, cache.ports[ch].calibration, sizeof cal);
    unlock();
    if (!pending || !io.announce || !io.announce(ch, &identity, cal)) return;
    lock();
    if (cache.ports[ch].generation == generation &&
        cache.ports[ch].identity.cal_version == identity.cal_version &&
        !strcmp(cache.ports[ch].identity.fw_version, identity.fw_version)) {
        work[ch].announced_crc = identity.cal_version;
        memcpy(work[ch].announced_id, identity.device_id, sizeof identity.device_id);
        memcpy(work[ch].announced_fw, identity.fw_version, sizeof identity.fw_version);
        work[ch].announce_pending = false;
    }
    unlock();
}
void sensor_inventory_announce_pending(uint8_t ch)
{ if (ch < SENSOR_PORTS) announce(ch); }
bool sensor_inventory_accept(uint8_t ch, uint8_t type, const void *bytes, size_t size)
{
    if (ch >= SENSOR_PORTS || !bytes) return false;
    sensor_stamp_t observed = stamp();
    lock(); bool ok = accept_locked(ch, type, bytes, size, observed); unlock();
    if (ok && type == 1) announce(ch);
    return ok;
}

/* Refresh admission shares the metadata lock with every veto/acquisition
 * reservation. Blocking UART and all yielding occur after releasing it. */
static bool reserve(bool acquisition, uint8_t ch, bool run_admission)
{
    if (!initialized) return true;
    int64_t started = io.now_us(); bool waited = false;
    for (;;) {
        lock();
        if (!refreshing && !recovering && !legacy_reading) {
            /* Busy excludes inspection. An explicit subsequent acquisition
             * may retry that channel through its existing UART mutex after a
             * lost ACK; the observation guard must not deadlock recovery. */
            (void)idle_now_locked();
            if (acquisition) cache.busy |= (uint8_t)(1u << ch); else ++vetoes;
            if (run_admission && waited) {
                increment(&cache.refresh.run_start_waits);
                maximum(&cache.refresh.run_start_wait_max_us, elapsed_us(started));
            }
            unlock(); sensor_inventory_wake(); return true;
        }
        waited |= refreshing || recovering;
        unlock();
        if (elapsed_us(started) >= 1000000u || !io.yield_ms) return false;
        io.yield_ms(1);
    }
}
uint64_t sensor_inventory_acquire_run(uint8_t ch)
{
    if (ch >= SENSOR_PORTS) return 0;
    if (!initialized) return 1;
    lock();
    if (runs[ch].epoch == UINT64_MAX) { unlock(); return 0; }
    struct run_state previous = runs[ch];
    uint8_t busy = cache.busy & (1u << ch), stalled = cache.stalled & (1u << ch);
    (void)idle_now_locked();
    uint64_t epoch = ++runs[ch].epoch;
    /* Fence an in-flight old response before waiting for its UART lease.
     * The new run cannot send until that bounded lease has been released. */
    runs[ch].due_ms = runs[ch].window_end_ms = INT64_MAX;
    runs[ch].attempts = 0;
    runs[ch].refusal_ms = 1000;
    cache.busy |= (uint8_t)(1u << ch); cache.stalled &= (uint8_t)~(1u << ch);
    unlock();
    bool admitted = reserve(true, ch, true);
    lock();
    if (!admitted && runs[ch].epoch == epoch) {
        runs[ch] = previous; runs[ch].epoch = epoch;
        cache.busy = (cache.busy & (uint8_t)~(1u << ch)) | busy;
        cache.stalled = (cache.stalled & (uint8_t)~(1u << ch)) | stalled;
    }
    bool current = runs[ch].epoch == epoch;
    unlock(); sensor_inventory_wake();
    return admitted && current ? epoch : 0;
}
bool sensor_inventory_acquire(uint8_t ch)
{ return sensor_inventory_acquire_run(ch) != 0; }
uint64_t sensor_inventory_run_epoch(uint8_t ch)
{ lock(); uint64_t epoch = ch < SENSOR_PORTS ? runs[ch].epoch : 0; unlock(); return epoch; }
bool sensor_inventory_run_current(uint8_t ch, uint64_t epoch)
{
    lock(); bool ok = ch < SENSOR_PORTS && epoch && runs[ch].epoch == epoch && (cache.busy & (1u << ch));
    unlock(); return ok;
}
void sensor_inventory_run_window(uint8_t ch, uint64_t epoch, uint32_t expected_ms)
{
    lock();
    if (ch < SENSOR_PORTS && runs[ch].epoch == epoch && (cache.busy & (1u << ch)))
        runs[ch].due_ms = runs[ch].window_end_ms = now_ms() + expected_ms;
    unlock(); sensor_inventory_wake();
}
void sensor_inventory_complete_epoch(uint8_t ch, uint64_t epoch)
{
    if (ch >= SENSOR_PORTS) return;
    lock();
    if (runs[ch].epoch == epoch) {
        (void)idle_now_locked();
        cache.busy &= (uint8_t)~(1u << ch); cache.stalled &= (uint8_t)~(1u << ch);
        runs[ch].due_ms = INT64_MAX;
    }
    unlock(); sensor_inventory_wake();
}
void sensor_inventory_complete(uint8_t ch)
{ sensor_inventory_complete_epoch(ch, sensor_inventory_run_epoch(ch)); }
void sensor_inventory_veto_begin(void)
{
    /* Maintenance must wait for the one <=900ms refresh, never proceed with a
     * failed reservation. A task preemption is not permission to reset a bank. */
    while (!reserve(false, 0, false)) if (io.yield_ms) io.yield_ms(1);
}
void sensor_inventory_run_begin(void)
{
    /* The runner reserves the whole job before any leaf is triggered. Count
     * its admission here, where an already admitted refresh actually delays it. */
    while (!reserve(false, 0, true)) if (io.yield_ms) io.yield_ms(1);
}
void sensor_inventory_veto_end(void)
{ lock(); (void)idle_now_locked(); if (vetoes) --vetoes; unlock(); sensor_inventory_wake(); }
bool sensor_inventory_legacy_read_begin(uint8_t ch)
{
    lock();
    bool ok = ch < SENSOR_PORTS && !blocked_locked(ch) && !refreshing && !recovering && !legacy_reading;
    if (ok) legacy_reading = true;
    unlock(); return ok;
}
void sensor_inventory_legacy_read_end(void)
{ lock(); legacy_reading = false; unlock(); sensor_inventory_wake(); }
bool sensor_inventory_busy(void)
{ lock(); bool busy = cache.busy != 0; unlock(); return busy; }
bool sensor_inventory_blocked(uint8_t ch)
{ lock(); bool busy = ch >= SENSOR_PORTS || blocked_locked(ch); unlock(); return busy; }
bool sensor_inventory_refresh_owned(void)
{ lock(); bool busy = refreshing || recovering; unlock(); return busy; }

static int64_t due_locked(void)
{
    int64_t due = INT64_MAX;
    for (unsigned ch = 0; ch < SENSOR_PORTS; ++ch)
        if (!blocked_locked((uint8_t)ch) && work[ch].due_ms < due) due = work[ch].due_ms;
    return due < next_block_ms ? next_block_ms : due;
}
uint32_t sensor_inventory_wait_ms(void)
{
    if (!initialized || (!io.inspect && !io.recover)) return UINT32_MAX;
    lock();
    if (vetoes || refreshing || recovering || legacy_reading) { unlock(); return UINT32_MAX; }
    int64_t idle = idle_now_locked(), real = now_ms(), d = INT64_MAX;
    int64_t due = io.inspect ? due_locked() : INT64_MAX;
    if (due != INT64_MAX) d = due - idle;
    /* Wake once when the bank-wide window ends, even when no recovery
     * adapter is present. Other ports then become eligible independently. */
    int64_t bank = bank_until_locked();
    if (bank > real && bank != INT64_MAX && bank - real < d) d = bank - real;
    if (io.recover) for (unsigned ch = 0; ch < SENSOR_PORTS; ++ch)
        if ((cache.busy & (1u << ch)) && !(cache.stalled & (1u << ch)) &&
            runs[ch].due_ms != INT64_MAX && runs[ch].due_ms - real < d) d = runs[ch].due_ms - real;
    unlock();
    if (d == INT64_MAX) return UINT32_MAX;
    return d <= 0 ? 0 : d > SENSOR_REFRESH_MS ? SENSOR_REFRESH_MS : (uint32_t)d;
}
static void started(uint8_t ch)
{
    lock();
    if (refreshing && ch < SENSOR_PORTS) increment(&cache.refresh.wire_transactions[ch]);
    unlock();
}
static bool supports_spectral(const ambit_device_info_t *e)
{
    unsigned a = 0, b = 0, c = 0;
    return e->valid && sscanf(e->fw_version, "%u.%u.%u", &a, &b, &c) == 3 &&
        (a > 1 || (a == 1 && b >= 2));
}
void sensor_inventory_refresh(void)
{
    if (!initialized || !io.inspect) return;
    lock();
    if (vetoes || refreshing || recovering || legacy_reading) {
        if (cache.busy) increment(&cache.refresh.deferred_acquisition);
        unlock(); return;
    }
    int64_t now = idle_now_locked();
    int64_t due = due_locked();
    if (now < due) {
        if (cache.busy && due == INT64_MAX) increment(&cache.refresh.deferred_acquisition);
        unlock(); return;
    }
    unsigned ch = SENSOR_PORTS;
    for (unsigned i = 0; i < SENSOR_PORTS; ++i)
        if (!blocked_locked((uint8_t)i) && (ch == SENSOR_PORTS || work[i].due_ms < work[ch].due_ms)) ch = i;
    if (ch == SENSOR_PORTS) { unlock(); return; }
    unsigned phase = work[ch].phase;
    uint8_t type = phase == 0 ? 0 : phase == 1 ? 2 : phase == 2 ? 1 : 4;
    uint32_t generation = cache.ports[ch].generation;
    uint32_t epoch = work[ch].epoch;
    refreshing = true;
    unlock();
    unsigned char bytes[SENSOR_CAL_BYTES] = {0};
    size_t got = 0, size = type == 0 ? 0 : type == 2 ? 48 : type == 1 ? 140 : 132;
    if (io.sample) io.sample();
    sensor_read_result_t result = io.inspect((uint8_t)ch, type, bytes, size, &got, SENSOR_BLOCK_MS, started);
    sensor_stamp_t observed = stamp();
    lock();
    sensor_port_snapshot_t *p = &cache.ports[ch];
    /* An explicit reset/invalidate can supersede in-flight data; both the
     * generation and the planned phase must still match before publication. */
    if (p->generation == generation && work[ch].epoch == epoch && work[ch].phase == phase) {
        if (type == 0) {
            if (result == SENSOR_READ_ABSENT) {
                invalidate_locked((uint8_t)ch); p = &cache.ports[ch];
                p->seen = true; p->present = false; p->presence_stamp = observed;
                work[ch].due_ms = idle_now_locked() + SENSOR_REFRESH_MS;
            } else if (result == SENSOR_READ_OK) {
                p->seen = p->present = true; p->presence_stamp = observed;
                work[ch].phase = 1; work[ch].due_ms = idle_now_locked();
            } else { p->revalidation_failed = true; work[ch].due_ms = idle_now_locked() + SENSOR_REFRESH_MS; }
        } else if (type == 2) {
            if (result == SENSOR_READ_OK && accept_locked((uint8_t)ch, type, bytes, got, observed)) {
                work[ch].phase = 2; work[ch].due_ms = idle_now_locked();
            } else { p->revalidation_failed = true; work[ch].phase = 0; work[ch].due_ms = idle_now_locked() + SENSOR_REFRESH_MS; }
        } else if (type == 1) {
            if (result == SENSOR_READ_OK) (void)accept_locked((uint8_t)ch, type, bytes, got, observed);
            bool spectral = !work[ch].no_spec && supports_spectral(&p->identity) && idle_now_locked() >= work[ch].spec_due_ms;
            work[ch].phase = spectral ? 3 : 0;
            work[ch].due_ms = idle_now_locked() + (spectral ? 0 : SENSOR_REFRESH_MS);
        } else {
            bool valid = result == SENSOR_READ_OK && accept_locked((uint8_t)ch, type, bytes, got, observed);
            if (result == SENSOR_READ_UNSUPPORTED) work[ch].no_spec = true;
            work[ch].spec_due_ms = idle_now_locked() + (valid ? SENSOR_REFRESH_MS : SENSOR_SPEC_RETRY_MS);
            work[ch].phase = 0; work[ch].due_ms = idle_now_locked() + SENSOR_REFRESH_MS;
            /* C1: only spectral availability/retry bookkeeping changes. */
        }
    }
    next_block_ms = idle_now_locked() + 250;
    refreshing = false;
    unlock();
    /* Existing measurement identity lookups emit pending inventory events.
     * Refresh never blocks the resident worker behind an event-store fsync. */
    if (io.sample) io.sample();
}
static void recovery_started(uint8_t ch)
{
    lock();
    if (recovering && ch < SENSOR_PORTS) {
        recovery_transmitted = true;
        increment(&cache.recovery_wire_transactions[ch]);
    }
    unlock();
}
static bool recovery_poll(const ambit_device_info_t *e)
{
    unsigned a = 0, b = 0, c = 0;
    return e->valid && sscanf(e->fw_version, "%u.%u.%u", &a, &b, &c) == 3 &&
        (a > 1 || (a == 1 && (b > 1 || (b == 1 && c >= 2))));
}
static sensor_recovery_result_t recover_locked(unsigned ch, bool publish)
{
    uint64_t epoch = runs[ch].epoch;
    bool poll = recovery_poll(&cache.ports[ch].identity);
    recovering = true;
    recovery_transmitted = false;
    unlock();
    int64_t began = io.now_us();
    if (io.sample) io.sample();
    uint32_t sampling_us = elapsed_us(began);
    sensor_recovery_result_t result = sampling_us >= SENSOR_BLOCK_MS * 1000u ? SENSOR_RECOVERY_REFUSED :
        io.recover((uint8_t)ch, epoch, poll, (SENSOR_BLOCK_MS * 1000u - sampling_us) / 1000u,
                   sensor_inventory_run_current, recovery_started);
    if (elapsed_us(began) > SENSOR_BLOCK_MS * 1000u) result = SENSOR_RECOVERY_REFUSED;
    lock();
    if (publish && runs[ch].epoch == epoch && (cache.busy & (1u << ch))) {
        if (!recovery_transmitted || result == SENSOR_RECOVERY_REFUSED) {
            /* A refused lock/budget/grace is not a failed leaf exchange.
             * Back off without consuming its bounded on-wire retry budget. */
            runs[ch].due_ms = now_ms() + runs[ch].refusal_ms;
            runs[ch].refusal_ms = runs[ch].refusal_ms < 30000 ? runs[ch].refusal_ms * 2u : 60000;
        } else if (result == SENSOR_RECOVERY_PROVEN) {
            (void)idle_now_locked();
            cache.busy &= (uint8_t)~(1u << ch);
            cache.stalled &= (uint8_t)~(1u << ch);
            runs[ch].due_ms = INT64_MAX;
        } else if (++runs[ch].attempts >= SENSOR_RECOVERY_ATTEMPTS) {
            cache.stalled |= (uint8_t)(1u << ch);
            runs[ch].due_ms = INT64_MAX;
        } else {
            runs[ch].due_ms = now_ms() + (int64_t)60000 * runs[ch].attempts;
        }
        if (recovery_transmitted && result != SENSOR_RECOVERY_REFUSED) runs[ch].refusal_ms = 1000;
        if (runs[ch].due_ms < runs[ch].window_end_ms) runs[ch].due_ms = runs[ch].window_end_ms;
    }
    if (runs[ch].epoch != epoch) result = SENSOR_RECOVERY_REFUSED;
    recovering = false;
    unlock(); sensor_inventory_wake();
    if (io.sample) io.sample();
    return result;
}
void sensor_inventory_tick(void)
{
    if (!initialized) return;
    lock();
    unsigned ch = SENSOR_PORTS;
    if (io.recover && !vetoes && !refreshing && !recovering && !legacy_reading) {
        for (unsigned i = 0; i < SENSOR_PORTS; ++i)
            if ((cache.busy & (1u << i)) && !(cache.stalled & (1u << i)) &&
                runs[i].due_ms != INT64_MAX && now_ms() >= runs[i].due_ms &&
                (ch == SENSOR_PORTS || runs[i].due_ms < runs[ch].due_ms)) ch = i;
    }
    if (ch == SENSOR_PORTS) { unlock(); sensor_inventory_refresh(); return; }
    (void)recover_locked(ch, true);
}
#if CONFIG_AMBYTE_SENSOR_RECOVERY_HIL
sensor_recovery_result_t sensor_inventory_recovery_hil(uint8_t ch)
{
    if (ch >= SENSOR_PORTS || !initialized || !io.recover) return SENSOR_RECOVERY_REFUSED;
    /* Bench-only: deliberately exercise the SAME block on an idle or running
     * leaf. The live-run case must fail without changing the acquisition.
     * Ordinary release builds contain neither this entry point nor its CLI. */
    sensor_inventory_veto_begin();
    lock(); bool busy = cache.busy & (1u << ch); unlock();
    uint64_t synthetic_epoch = 0;
    if (!busy) {
        uint64_t epoch = sensor_inventory_acquire_run(ch);
        synthetic_epoch = epoch;
        if (!epoch) { sensor_inventory_veto_end(); return SENSOR_RECOVERY_REFUSED; }
        sensor_inventory_run_window(ch,epoch,0);
    }
    lock();
    sensor_recovery_result_t result;
    if (refreshing || recovering || legacy_reading) { unlock(); result=SENSOR_RECOVERY_REFUSED; }
    else result=recover_locked(ch, false);
    /* A synthetic idle test never becomes an orphan acquisition on a failed
     * block. An existing real run is observe-only: no automatic retry budget,
     * expected-window or busy-state mutation from a forced bench check. */
    if (synthetic_epoch) sensor_inventory_complete_epoch(ch, synthetic_epoch);
    sensor_inventory_veto_end();
    return result;
}
#endif

void sensor_inventory_note_stdout(uint32_t us)
{ lock(); maximum(&cache.refresh.stdout_hold_max_us, us); unlock(); }
void sensor_inventory_note_pin(uint32_t allocated)
{ lock(); cache.refresh.pin_heap_bytes = allocated; maximum(&cache.refresh.pin_heap_peak_bytes, allocated); unlock(); }
void sensor_inventory_note_job_wait(uint32_t us)
{ lock(); increment(&cache.refresh.job_start_waits); maximum(&cache.refresh.job_start_wait_max_us, us); unlock(); }
void sensor_inventory_recovery_discard(uint8_t ch, uint32_t bytes)
{
    if (ch >= SENSOR_PORTS) return;
    lock();
    uint32_t *total = &cache.recovery_discarded_bytes[ch];
    *total = bytes > UINT32_MAX - *total ? UINT32_MAX : *total + bytes;
    unlock();
}

void sensor_inventory_recovery_boot_idle(uint8_t ch)
{ if (ch < SENSOR_PORTS) { lock(); increment(&cache.recovery_boot_idle[ch]); unlock(); } }

void sensor_inventory_recovery_prime_notice(uint8_t ch)
{ if (ch < SENSOR_PORTS) { lock(); increment(&cache.recovery_prime_notices[ch]); unlock(); } }
