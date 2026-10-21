/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "sensor_inventory.h"
/* Include the complete production unit to seed exhaustion, never a copied model. */
#include "../../components/sensor_inventory/sensor_inventory.c"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static int64_t us;
static unsigned announcements, attempts[4][5];
static bool absent[4];
static unsigned spec_failure, cal_failure;
static int64_t spec_last_idle[4];
static bool invalidate_inflight;
static int64_t utc_base = 1792531200000LL;
static uint8_t quality = 3;
static bool suspect_clock;
static uint8_t firmware[4][48], calibration[4][140], spectral[4][132];
static void host_lock(void) { assert(!pthread_mutex_lock(&mutex)); }
static void host_unlock(void) { assert(!pthread_mutex_unlock(&mutex)); }
static int64_t now(void) { return us; }
static int64_t utc(void) { return utc_base + us / 1000; }
static uint8_t clock_source(bool *suspect) { *suspect = suspect_clock; return quality; }
static void delay(uint32_t ms) { us += (int64_t)ms * 1000; }
static bool host_announce(uint8_t ch, const ambit_device_info_t *info, const void *cal)
{ (void)ch; assert(info->valid && cal); ++announcements; return true; }
static sensor_read_result_t inspect(uint8_t ch, uint8_t type, void *out, size_t size,
    size_t *got, uint32_t timeout, void (*started)(uint8_t))
{
    assert(timeout <= 1000); assert(!sensor_inventory_busy());
    started(ch);
    if (type==4 && spec_failure==2 && attempts[ch][type])
        assert(idle_now_locked()-spec_last_idle[ch]>=SENSOR_SPEC_RETRY_MS);
    if (type==4) spec_last_idle[ch]=idle_now_locked();
    ++attempts[ch][type]; *got = 0;
    if (absent[ch]) { delay(timeout); return type ? SENSOR_READ_FAILED : SENSOR_READ_ABSENT; }
    if (type == 1 && cal_failure) { delay(timeout); return SENSOR_READ_FAILED; }
    if (invalidate_inflight) { invalidate_inflight = false; sensor_inventory_invalidate(ch); }
    if (type == 4 && spec_failure) {
        delay(timeout);
        return spec_failure == 1 ? SENSOR_READ_UNSUPPORTED : SENSOR_READ_FAILED;
    }
    if (type) {
        assert(size == (type == 1 ? 140u : type == 2 ? 48u : 132u));
        memcpy(out, type == 1 ? calibration[ch] : type == 2 ? firmware[ch] : spectral[ch], size); *got = size;
    }
    delay(1); return SENSOR_READ_OK;
}
static bool write_json(void *context, const void *p, size_t n)
{ (void)context; return fwrite(p, 1, n, stdout) == n; }
static void output(void)
{
    sensor_snapshot_t snapshot; assert(sensor_inventory_snapshot(&snapshot));
    assert(sensor_snapshot_json(&snapshot, write_json, NULL)); puts("");
}
static void put_f32(uint8_t *p, float f) { memcpy(p, &f, 4); }
static void populate(void)
{
    for (unsigned ch = 0; ch < 4; ++ch) {
        firmware[ch][0] = 1; firmware[ch][1] = 4; firmware[ch][28] = 7;
        for (unsigned i = 8; i < 14; ++i) firmware[ch][i] = (uint8_t)(16*ch + i);
        snprintf((char *)calibration[ch], 20, "Sensor %u", ch);
        for (unsigned i = 0; i < 14; ++i) { int32_t value = -(int32_t)(1000*ch + i); memcpy(calibration[ch]+20+4*i, &value, 4); }
        for (unsigned i = 0; i < 6; ++i) { uint32_t value = 100*ch+i; memcpy(calibration[ch]+76+4*i, &value, 4); }
        for (unsigned i = 100; i <= 136; i += 4) put_f32(calibration[ch]+i, (float)ch + 0.25f);
        spectral[ch][0] = 1;
        for (unsigned i = 4; i <= 128; i += 4) put_f32(spectral[ch]+i, (float)ch + (float)i/4);
    }
    const sensor_inventory_ports_t ports = {.now_us = now, .utc_ms = utc, .clock = clock_source,
        .lock = host_lock, .unlock = host_unlock, .yield_ms = delay, .inspect = inspect, .announce = host_announce};
    sensor_inventory_init(&ports, 0x123456789abcdefULL);
}
static void fill(void)
{
    for (unsigned ch = 0; ch < 4; ++ch) {
        assert(sensor_inventory_accept(ch, 2, firmware[ch], 48));
        assert(sensor_inventory_accept(ch, 1, calibration[ch], 140));
        assert(sensor_inventory_accept(ch, 4, spectral[ch], 132));
    }
}
static void step(void)
{
    uint32_t wait = sensor_inventory_wait_ms(); assert(wait != UINT32_MAX);
    delay(wait); sensor_inventory_refresh();
}
int main(int argc, char **argv)
{
    assert(argc >= 2); populate();
    const char *scenario = argv[1];
    if (!strcmp(scenario, "version_gate")) {
        const uint8_t versions[][3]={{0,0,0},{0,0,6},{1,1,2},{1,2,0},{1,3,1},{1,4,0}};
        for (unsigned v=0;v<6;++v) {
            populate(); memset(attempts,0,sizeof attempts);
            for (unsigned ch=0;ch<4;++ch) memcpy(firmware[ch],versions[v],3);
            for (unsigned i=0;i<20;++i) step();
            assert(v<3 ? attempts[0][4]==0 : attempts[0][4]>0);
        }
        output(); return 0;
    }
    if (!strcmp(scenario, "clocks")) {
        for (quality = 0; quality <= 3; ++quality) {
            utc_base = quality ? 1792531200000LL : 100; suspect_clock = quality == 2;
            fill(); output(); delay(1000);
        }
        /* A wall-clock rollback changes UTC only; observation uptime/age stay monotonic. */
        utc_base -= 3600000; fill(); output();
        uint64_t first_boot = cache.boot_id; populate();
        sensor_inventory_init(&io, first_boot + 1); assert(cache.boot_id != first_boot); return 0;
    }
    if (!strcmp(scenario, "zero_crc")) {
        memset(calibration[0], 0, 140);
        memcpy(calibration[0]+136, "\x7b\x39\xea\x1a", 4);
        assert(sensor_inventory_crc32(calibration[0], 140) == 0);
        fill(); assert(cache.ports[0].have_cal && cache.ports[0].identity.cal_version == 0);
        unsigned count = announcements;
        for (unsigned i = 0; i < 100; ++i) { step(); sensor_inventory_announce_pending(0); }
        assert(announcements == count); output(); return 0;
    }
    if (!strcmp(scenario, "exhaustion")) {
        cache.ports[0].generation = UINT32_MAX - 1;
        assert(sensor_inventory_accept(0, 2, firmware[0], 48));
        sensor_inventory_invalidate(0);
        assert(!sensor_inventory_accept(0, 2, firmware[1], 48));
        assert(!cache.ports[0].identity.valid && cache.ports[0].generation == UINT32_MAX);
        output(); return 0;
    }
    if (!strcmp(scenario, "epoch")) {
        fill(); work[0].phase = 1; invalidate_inflight = true;
        step(); assert(!cache.ports[0].identity.valid && !cache.ports[0].have_cal);
        assert(cache.ports[0].generation == 1); output(); return 0;
    }
    if (!strcmp(scenario, "bad_format")) {
        spectral[0][0] = 2;
        for (unsigned i = 0; !attempts[0][4] && i < 20; ++i) step();
        assert(!cache.ports[0].have_spec && cache.ports[0].identity.valid);
        unsigned count = attempts[0][4];
        for (unsigned i = 0; i < 50; ++i) step();
        assert(attempts[0][4] == count); output(); return 0;
    }
    if (!strcmp(scenario, "cal_flap")) {
        fill(); sensor_port_snapshot_t saved = cache.ports[0];
        for (unsigned i = 0; i < 2000; ++i) {
            cal_failure = i % 2;
            do { step(); } while (work[0].phase != 0);
            sensor_inventory_announce_pending(0);
            assert(cache.ports[0].generation == saved.generation);
            assert(!memcmp(cache.ports[0].calibration, saved.calibration, 140));
        }
        assert(announcements == 4); output(); return 0;
    }
    if (!strcmp(scenario, "seeded")) {
        uint32_t seed = argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 10) : 1;
        unsigned kinds[8] = {0}; fill();
        for (unsigned i = 0; i < 4000; ++i) {
            seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
            unsigned ch = (seed >> 5) % 4, action = seed % 8; ++kinds[action];
            sensor_port_snapshot_t before = cache.ports[ch];
            if (action == 0) { /* Physical replacement clears calibration/config. */
                firmware[ch][13] ^= 0x80;
                assert(sensor_inventory_accept(ch, 2, firmware[ch], 48));
                assert(cache.ports[ch].generation == before.generation + 1);
                assert(!cache.ports[ch].have_cal && !cache.ports[ch].identity.gains_set);
            } else if (action == 1) { /* Failed presence, then same-MAC reconnect. */
                absent[ch] = true; work[ch].phase = 0; work[ch].due_ms = -1; step();
                assert(!cache.ports[ch].present && !cache.ports[ch].have_cal);
                absent[ch] = false; assert(sensor_inventory_accept(ch, 2, firmware[ch], 48));
                assert(cache.ports[ch].generation == before.generation + 1);
            } else if (action == 2) {
                assert(sensor_inventory_acquire(ch));
                unsigned counts[4][5]; memcpy(counts, attempts, sizeof counts);
                delay(seed % 10000); sensor_inventory_refresh();
                assert(!memcmp(counts, attempts, sizeof counts));
                assert(!memcmp(&before, &cache.ports[ch], sizeof before));
                sensor_inventory_complete(ch);
            } else if (action == 3) {
                sensor_inventory_veto_begin(); sensor_inventory_refresh();
                assert(sensor_inventory_wait_ms() == UINT32_MAX); sensor_inventory_veto_end();
            } else if (action == 4) {
                assert(sensor_inventory_accept(ch, 1, calibration[ch], 140));
            } else if (action == 5) {
                sensor_inventory_invalidate(ch); work[ch].phase = 1; work[ch].due_ms = -1;
                invalidate_inflight = true; step(); assert(!cache.ports[ch].identity.valid);
                assert(sensor_inventory_accept(ch, 2, firmware[ch], 48));
            } else if (action == 6) {
                cal_failure = 1; work[ch].phase = 2; work[ch].due_ms = -1; step(); cal_failure = 0;
                assert(!memcmp(before.calibration, cache.ports[ch].calibration, 140));
            } else {
                uint8_t gains[6] = {1,2,3,4,5,6}; sensor_inventory_config(ch, gains, NULL);
                assert(cache.ports[ch].identity.gains_set);
            }
            for (unsigned p = 0; p < 4; ++p) {
                assert(cache.ports[p].identity.valid);
                assert(!cache.ports[p].have_cal || cache.ports[p].identity.cal_version == sensor_inventory_crc32(cache.ports[p].calibration, 140));
            }
        }
        for (unsigned i = 0; i < 8; ++i) assert(kinds[i] > 300);
        output(); return 0;
    }
    if (!strcmp(scenario, "empty")) { output(); return 0; }
    if (!strcmp(scenario, "filled")) { fill(); output(); return 0; }
    if (!strcmp(scenario, "stale")) { fill(); delay(60001); output(); return 0; }
    if (!strcmp(scenario, "fresh_boundary")) { fill(); delay(60000); output(); return 0; }
    if (!strcmp(scenario, "absent")) { absent[0] = true; step(); output(); assert(us <= 1000000); return 0; }
    if (!strcmp(scenario, "swap")) {
        fill(); uint8_t gains[6] = {1,2,3,4,5,6}; sensor_inventory_config(0, gains, NULL);
        assert(sensor_inventory_accept(0, 2, firmware[1], 48));
        ambit_device_info_t info; assert(sensor_inventory_cached(0, &info)); assert(!info.gains_set && info.cal_version == 0);
        assert(sensor_inventory_accept(0, 1, calibration[1], 140)); output(); return 0;
    }
    if (!strcmp(scenario, "busy")) {
        fill(); uint8_t gains[6] = {1,2,3,4,5,6}, currents[3] = {7,8,9};
        sensor_inventory_config(0, gains, currents);
        sensor_snapshot_t before, after; assert(sensor_inventory_snapshot(&before));
        assert(sensor_inventory_acquire(0)); assert(sensor_inventory_wait_ms() == UINT32_MAX);
        for (unsigned i = 0; i < 1000; ++i) sensor_inventory_refresh();
        assert(sensor_inventory_snapshot(&after)); assert(!memcmp(before.ports, after.ports, sizeof before.ports));
        assert(after.refresh.deferred_acquisition == 1000);
        for (unsigned i = 0; i < 4; ++i) assert(!after.refresh.wire_transactions[i]);
        output(); sensor_inventory_complete(0); assert(sensor_inventory_wait_ms() == 0); return 0;
    }
    if (!strcmp(scenario, "veto")) {
        fill(); sensor_inventory_veto_begin(); assert(sensor_inventory_wait_ms() == UINT32_MAX);
        sensor_inventory_refresh(); sensor_inventory_veto_begin(); sensor_inventory_refresh(); sensor_inventory_veto_end();
        sensor_inventory_refresh(); sensor_inventory_veto_end();
        sensor_snapshot_t s; assert(sensor_inventory_snapshot(&s));
        for (unsigned i = 0; i < 4; ++i) assert(!s.refresh.wire_transactions[i]);
        assert(sensor_inventory_wait_ms() == 0); output(); return 0;
    }
    if (!strcmp(scenario, "reconnect")) {
        fill(); sensor_inventory_invalidate(0); output();
        assert(sensor_inventory_accept(0, 2, firmware[0], 48)); output(); return 0;
    }
    if (!strcmp(scenario, "reset")) {
        fill(); sensor_inventory_reset(); output();
        assert(sensor_inventory_accept(0, 2, firmware[0], 48)); output(); return 0;
    }
    if (!strcmp(scenario, "spec_bare") || !strcmp(scenario, "spec_silent")) {
        spec_failure = !strcmp(scenario, "spec_bare") ? 1 : 2;
        sensor_snapshot_t before, after;
        for (unsigned i = 0; !attempts[0][4] && i < 20; ++i) {
            assert(sensor_inventory_snapshot(&before)); step();
        }
        assert(sensor_inventory_snapshot(&after));
        assert(!memcmp(&before.ports[0], &after.ports[0], sizeof before.ports[0]));
        assert(attempts[0][4] == 1);
        /* Busy elapsed wall time does not consume the retry's idle budget. */
        assert(sensor_inventory_acquire(0)); delay(1200000); sensor_inventory_complete(0);
        for (unsigned i = 0; i < 50; ++i) step();
        assert(attempts[0][4] == 1);
        if (spec_failure == 2) { delay(600000); for (unsigned i = 0; i < 40; ++i) step(); assert(attempts[0][4] >= 2 && attempts[0][4] <= 3); }
        else { delay(1200000); for (unsigned i = 0; i < 40; ++i) step(); assert(attempts[0][4] == 1); }
        output(); return 0;
    }
    if (!strcmp(scenario, "stable")) {
        fill(); assert(announcements == 4);
        for (unsigned i = 0; i < 17000; ++i) { step(); for (unsigned ch = 0; ch < 4; ++ch) sensor_inventory_announce_pending(ch); }
        assert(announcements == 4); assert(attempts[0][2] >= 1000); output(); return 0;
    }
    if (!strcmp(scenario, "old")) {
        firmware[0][1] = 1;
        for (unsigned i = 0; i < 16; ++i) step();
        assert(!attempts[0][4]); output(); return 0;
    }
    if (!strcmp(scenario, "bad_values")) {
        memset(calibration[0], 1, 20); calibration[0][0] = 0xc0;
        put_f32(calibration[0]+100, __builtin_nanf("")); put_f32(spectral[0]+4, __builtin_inff());
        fill(); output(); return 0;
    }
    if (!strcmp(scenario, "slice")) {
        fill(); sensor_snapshot_t s; assert(sensor_inventory_snapshot(&s));
        unsigned char all[SENSOR_SNAPSHOT_MAX], part[576]; size_t total = 0;
        size_t n = sensor_snapshot_slice(&s, 0, all, sizeof all, &total); assert(n == total && n > 576);
        for (size_t off = 0; off < total; off += sizeof part) {
            size_t full = 0, got = sensor_snapshot_slice(&s, off, part, sizeof part, &full);
            assert(full == total && got == (total-off < sizeof part ? total-off : sizeof part));
            assert(!memcmp(all+off, part, got));
        }
        assert(!sensor_snapshot_slice(&s, total, part, sizeof part, NULL)); output(); return 0;
    }
    fprintf(stderr, "unknown scenario %s\n", scenario); return 2;
}
