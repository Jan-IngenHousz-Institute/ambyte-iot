/* Production-glue test of components/sd_card/sd_diag.c (contract C-27/C-28):
 * the real boot/persist code against an NVS stand-in whose open/get/set/commit
 * can fail or return missing/malformed data. RTC_NOINIT is an ordinary global
 * here, so a "CPU reset" keeps it and a "power-on" is simulated by scrambling. */
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "esp_system.h"
#include "nvs.h"
#include "sd_diag.h"

atomic_int sdwi_mtx_contended;       /* bumped by glue_stubs/freertos/semphr.h */

static int fails;
#define CHECK(name, cond) do { bool ok_ = (cond); printf("{\"check\":\"%s\",\"ok\":%s}\n", name, ok_ ? "true" : "false"); if (!ok_) fails++; } while (0)

/* ── stubs ── */
static esp_reset_reason_t s_reason = ESP_RST_POWERON;
esp_reset_reason_t esp_reset_reason(void) { return s_reason; }
static int64_t s_now_us;
int64_t esp_timer_get_time(void) { return s_now_us; }

static unsigned char s_nvs[512];
static size_t s_nvs_len;             /* 0 = key absent */
static int f_open, f_get, f_set, f_commit, f_short;
static unsigned n_set, n_commit;
/* Interleaving control (section 9): the next set can be parked until the
 * test releases it; one set (1-based call index) can be made to fail; every
 * successful set's gen is checked against the previous one (nvs_set_blob is
 * the durable write on ESP-IDF 5.5, so an older gen landing later is exactly
 * the regression). */
static pthread_mutex_t s_st = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_cv = PTHREAD_COND_INITIALIZER;
static int s_pause_armed, s_paused, s_released;
static unsigned s_fail_set_at;
static uint32_t s_last_set_gen;
static int s_regressed;
esp_err_t nvs_open(const char *ns, nvs_open_mode_t m, nvs_handle_t *h) { (void)ns; (void)m; *h = 1; return f_open ? ESP_FAIL : ESP_OK; }
esp_err_t nvs_get_blob(nvs_handle_t h, const char *k, void *out, size_t *len)
{
    (void)h; (void)k;
    if (f_get) return ESP_FAIL;
    if (s_nvs_len == 0) return 0x1102;                  /* NOT_FOUND */
    size_t n = f_short ? s_nvs_len - 8 : s_nvs_len;     /* malformed: wrong length */
    if (n > *len) n = *len;
    memcpy(out, s_nvs, n);
    *len = n;
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char *k, const void *v, size_t len)
{
    (void)h; (void)k;
    pthread_mutex_lock(&s_st);
    unsigned me = ++n_set;
    bool fail = f_set || me == s_fail_set_at;
    if (s_pause_armed) {
        s_pause_armed = 0; s_paused = 1;
        pthread_cond_broadcast(&s_cv);
        while (!s_released) pthread_cond_wait(&s_cv, &s_st);   /* drops s_st: others may enter */
    }
    if (!fail) {
        uint32_t g = ((const sd_diag_block_t *)v)->gen;
        if (g < s_last_set_gen) s_regressed = 1;
        s_last_set_gen = g;
        memcpy(s_nvs, v, len); s_nvs_len = len;
    }
    pthread_mutex_unlock(&s_st);
    return fail ? ESP_FAIL : ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; n_commit++; return f_commit ? ESP_FAIL : ESP_OK; }
void nvs_close(nvs_handle_t h) { (void)h; }

static void power_on(void)
{
    s_reason = ESP_RST_POWERON;     /* the glue must not trust RTC contents after this */
    sd_diag_boot_early();
    sd_diag_boot_nvs();
}

static sd_diag_block_t get(void) { sd_diag_block_t b; sd_diag_get(&b); return b; }

/* ── section 9: heartbeat persist(false) racing the forced reboot persist ── */
static atomic_int s_b_done;
static void *persist_normal(void *a) { (void)a; sd_diag_persist(false); return NULL; }
static void *persist_forced(void *a) { (void)a; sd_diag_persist(true); atomic_store(&s_b_done, 1); return NULL; }

static void race_reset(void)
{
    s_nvs_len = 0; f_open = f_get = f_set = f_commit = f_short = 0;
    s_pause_armed = s_paused = s_released = 0; s_fail_set_at = 0;
    s_last_set_gen = 0; s_regressed = 0;
    atomic_store(&sdwi_mtx_contended, 0); atomic_store(&s_b_done, 0);
}

/* A = heartbeat snapshot of {write} parked inside nvs_set_blob; then an fsync
 * fault; then B = forced snapshot. A is released only once B is proven to be
 * waiting on the persist mutex (or, on unserialized glue, B already finished).
 * `fail_at` fails that set call (1 = A's, 2 = B's, 0 = none). Returns whether
 * B had to wait. */
static bool race(unsigned fail_at)
{
    s_fail_set_at = fail_at ? n_set + fail_at : 0;
    sd_diag_fault(SD_DIAG_W_SDLOG, SD_DIAG_OP_WRITE, 5);
    s_pause_armed = 1;
    pthread_t ta, tb;
    pthread_create(&ta, NULL, persist_normal, NULL);
    pthread_mutex_lock(&s_st);
    while (!s_paused) pthread_cond_wait(&s_cv, &s_st);
    pthread_mutex_unlock(&s_st);
    sd_diag_fault(SD_DIAG_W_SDLOG, SD_DIAG_OP_FSYNC, 5);        /* newer than A's copy */
    pthread_create(&tb, NULL, persist_forced, NULL);
    for (int i = 0; i < 5000 && atomic_load(&sdwi_mtx_contended) == 0 && !atomic_load(&s_b_done); i++) usleep(1000);
    bool waited = atomic_load(&sdwi_mtx_contended) > 0 && !atomic_load(&s_b_done);
    pthread_mutex_lock(&s_st);
    s_released = 1;
    pthread_cond_broadcast(&s_cv);
    pthread_mutex_unlock(&s_st);
    pthread_join(ta, NULL);
    pthread_join(tb, NULL);
    return waited;
}

static bool snapshot_is_live(const char *tag)
{
    sd_diag_block_t live = get(), saved;
    memcpy(&saved, s_nvs, sizeof saved);
    printf("{\"race\":\"%s\",\"saved_gen\":%u,\"live_gen\":%u,\"saved_fsync\":%u,\"live_fsync\":%u,\"saved_write\":%u,"
           "\"live_write\":%u,\"sets\":%u,\"regressed\":%d}\n", tag, (unsigned)saved.gen, (unsigned)live.gen,
           (unsigned)saved.cnt[SD_DIAG_W_SDLOG][SD_DIAG_OP_FSYNC], (unsigned)live.cnt[SD_DIAG_W_SDLOG][SD_DIAG_OP_FSYNC],
           (unsigned)saved.cnt[SD_DIAG_W_SDLOG][SD_DIAG_OP_WRITE], (unsigned)live.cnt[SD_DIAG_W_SDLOG][SD_DIAG_OP_WRITE],
           n_set, s_regressed);
    return s_nvs_len == sizeof saved && sd_diag_valid(&saved) && saved.gen == live.gen &&
           memcmp(saved.cnt, live.cnt, sizeof saved.cnt) == 0 && memcmp(saved.refused, live.refused, sizeof saved.refused) == 0;
}

int main(void)
{
    /* 1. power-on, key absent → inexact epoch 1 */
    power_on();
    sd_diag_block_t b = get();
    CHECK("missing snapshot: inexact epoch 1", b.exact == 0 && b.epoch == 1);
    CHECK("boot never writes NVS", n_set == 0 && n_commit == 0);

    /* 2. faults, snapshot persists */
    sd_diag_fault(SD_DIAG_W_SDLOG, SD_DIAG_OP_FSYNC, 5);
    s_now_us = 1000000;
    sd_diag_persist(false);
    CHECK("first change persisted", n_set == 1 && n_commit == 1 && s_nvs_len == sizeof(sd_diag_block_t));

    /* 3. set fails → not marked persisted → retried next call (even inside the interval) */
    sd_diag_fault(SD_DIAG_W_SDLOG, SD_DIAG_OP_WRITE, 5);
    f_set = 1; s_now_us = 2000000;
    sd_diag_persist(true);
    unsigned sets = n_set;
    f_set = 0; s_now_us = 3000000;
    sd_diag_persist(true);
    CHECK("failed set is retried", n_set == sets + 1);
    /* 4. commit fails → retried too */
    sd_diag_fault(SD_DIAG_W_SDLOG, SD_DIAG_OP_WRITE, 5);
    f_commit = 1; sd_diag_persist(true);
    unsigned commits = n_commit;
    f_commit = 0; sd_diag_persist(true);
    CHECK("failed commit is retried", n_commit == commits + 1);

    /* 5. CPU reset with a valid RTC block: counts continue, still inexact */
    s_reason = ESP_RST_SW;
    sd_diag_block_t before = get();
    sd_diag_boot_early(); sd_diag_boot_nvs();
    b = get();
    CHECK("cpu reset continues counts", b.cnt[SD_DIAG_W_SDLOG][SD_DIAG_OP_WRITE] == before.cnt[SD_DIAG_W_SDLOG][SD_DIAG_OP_WRITE] &&
          b.epoch == before.epoch && b.exact == before.exact);

    /* 6. power-on with a valid floor → counts >= floor, inexact, epoch+1 */
    unsigned floor_epoch = ((sd_diag_block_t *)s_nvs)->epoch;
    power_on();
    b = get();
    CHECK("valid floor: inexact, epoch+1, counts >= floor", b.exact == 0 && b.epoch == floor_epoch + 1 &&
          b.cnt[SD_DIAG_W_SDLOG][SD_DIAG_OP_FSYNC] >= 1);

    /* 7. failure modes at power-on all stay inexact */
    f_open = 1; power_on(); b = get(); CHECK("open fails: inexact", b.exact == 0); f_open = 0;
    f_get = 1; power_on(); b = get(); CHECK("get fails (unreadable): inexact", b.exact == 0); f_get = 0;
    f_short = 1; power_on(); b = get(); CHECK("malformed length: inexact", b.exact == 0 && b.epoch == 1); f_short = 0;
    ((sd_diag_block_t *)s_nvs)->crc ^= 1;
    power_on(); b = get(); CHECK("corrupt snapshot CRC: inexact", b.exact == 0 && b.epoch == 1);
    s_nvs_len = 0;
    power_on(); b = get(); CHECK("key missing again: inexact", b.exact == 0);
    /* 8. evaluator round-2 replay on the production glue: first boot, one
     * fault persisted (snapshot gen 2), power-on, one new fault: the normal
     * first-change snapshot and a forced one must both write; once written,
     * an unchanged block must be skipped by both. */
    s_nvs_len = 0; f_open = f_get = f_set = f_commit = f_short = 0;
    power_on();
    sd_diag_fault(SD_DIAG_W_SDLOG, SD_DIAG_OP_FSYNC, 5);
    s_now_us = 10000000;
    sd_diag_persist(false);
    unsigned floor_gen = ((sd_diag_block_t *)s_nvs)->gen;
    power_on();
    sd_diag_fault(SD_DIAG_W_SDLOG, SD_DIAG_OP_WRITE, 5);
    b = get();
    CHECK("glue: live gen above the restored floor gen", b.gen > floor_gen);
    unsigned w0 = n_set;
    sd_diag_persist(false);
    CHECK("glue: first change after floor merge is persisted", n_set == w0 + 1 &&
          ((sd_diag_block_t *)s_nvs)->cnt[SD_DIAG_W_SDLOG][SD_DIAG_OP_WRITE] == 1);
    sd_diag_fault(SD_DIAG_W_SDLOG, SD_DIAG_OP_WRITE, 5);
    unsigned w1 = n_set;
    sd_diag_persist(true);
    CHECK("glue: forced persist writes a pending change", n_set == w1 + 1);
    unsigned w2 = n_set;
    sd_diag_persist(false);
    sd_diag_persist(true);
    CHECK("glue: unchanged block after success is skipped", n_set == w2);

    /* 9. evaluator round-3 replay (C-28): the heartbeat snapshot and the
     * forced reboot snapshot overlap. The forced caller must wait for the
     * in-flight one, then decide and copy afresh: the durable snapshot is the
     * newest gen with every counter, and no older gen ever lands after it. */
    race_reset(); power_on(); s_now_us = 20000000;
    unsigned r0 = n_set;
    bool waited = race(0);
    CHECK("race: forced persist waited for the in-flight heartbeat snapshot", waited);
    CHECK("race: durable snapshot is the newest gen with all counters", snapshot_is_live("both_ok"));
    CHECK("race: no older snapshot landed after a newer one", !s_regressed);
    CHECK("race: forced caller re-evaluated and wrote the newer block", n_set == r0 + 2);
    unsigned r1 = n_set;
    sd_diag_persist(false);
    sd_diag_persist(true);
    CHECK("race: unchanged block after the race is skipped", n_set == r1);

    /* 9b. the in-flight heartbeat write FAILS: the waiting forced caller still
     * finds the change unpersisted and writes the newest block. */
    race_reset(); power_on(); s_now_us = 30000000;
    waited = race(1);
    CHECK("race/A fails: forced persist waited", waited);
    CHECK("race/A fails: forced caller wrote the newest block", snapshot_is_live("a_fails"));
    CHECK("race/A fails: no regression", !s_regressed);

    /* 9c. the forced write FAILS after the heartbeat one succeeded: the
     * durable floor is A's (older, never newer-then-older), the change stays
     * unpersisted, and the next forced call retries it. */
    race_reset(); power_on(); s_now_us = 40000000;
    waited = race(2);
    sd_diag_block_t a_saved;
    memcpy(&a_saved, s_nvs, sizeof a_saved);
    CHECK("race/B fails: forced persist waited", waited);
    CHECK("race/B fails: floor is the heartbeat snapshot, not a torn mix",
          sd_diag_valid(&a_saved) && a_saved.cnt[SD_DIAG_W_SDLOG][SD_DIAG_OP_WRITE] == 1 &&
          a_saved.cnt[SD_DIAG_W_SDLOG][SD_DIAG_OP_FSYNC] == 0 && a_saved.gen < get().gen);
    unsigned r2 = n_set;
    sd_diag_persist(true);
    CHECK("race/B fails: failed forced write is retried", n_set == r2 + 1 && snapshot_is_live("b_retry"));
    unsigned r3 = n_set;
    sd_diag_persist(true);
    CHECK("race/B fails: unchanged after retry is skipped", n_set == r3 && !s_regressed);

    printf("{\"fails\":%d}\n", fails);
    return fails != 0;
}
