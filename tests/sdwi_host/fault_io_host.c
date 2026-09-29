/* Host checks of the bench fault command's arming + wrappers (contract C-33..C-35):
 * compiles the PRODUCTION evq_hil_trace.c and evq_hil_io.c with ESP-IDF stubs. The
 * "CPU reset" (esp_rom_software_reset_system) longjmps back here with every global
 * kept, like RTC_NOINIT memory across a real CPU reset. */
#include <errno.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_system.h"
#include "event_log_hil.h"
#include "evq_hil_trace.h"
#include "evq_hil_io_impl.h"
#include "evq_hil_sdl.h"
#include "sd_diag.h"

FILE  *evq_hil_io_fopen_w(uint8_t w, const char *path, const char *mode);
int    evq_hil_io_fclose_w(uint8_t w, FILE *f);
size_t evq_hil_io_fwrite_w(uint8_t w, const void *p, size_t sz, size_t n, FILE *f);
int    evq_hil_io_fsync_w(uint8_t w, int fd);
int    evq_hil_io_rename_w(uint8_t w, const char *a, const char *b);
int    evq_hil_io_ftruncate_w(uint8_t w, int fd, off_t len);

#ifdef FIO_BASELINE_SEMANTICS
/* 4ec9cef semantics for the FIO-2 discrimination build: one targeted slot that
 * a second arm REPLACES, no path filter. */
static int base_set_slot(int slot, uint8_t w, uint8_t op, evq_iom_t m, unsigned nth, unsigned count, const char *path)
{
    (void)slot; (void)path;
    evq_arm_io_set(w, op, m, nth, count);
    return 0;
}
static evq_iom_t base_hit_p(uint8_t w, uint8_t op, const char *path, unsigned *nth, int *slot)
{
    (void)path;
    if (slot) *slot = 0;
    return evq_arm_io_hit(w, op, nth);
}
#define evq_arm_io_set_slot base_set_slot
#define evq_arm_io_hit_p    base_hit_p
#endif

static jmp_buf s_reset;
static esp_reset_reason_t s_reason = ESP_RST_POWERON;
esp_reset_reason_t esp_reset_reason(void) { return s_reason; }
void esp_rom_delay_us(uint32_t us) { (void)us; }
void esp_rom_software_reset_system(void) { longjmp(s_reset, 1); }
int64_t esp_timer_get_time(void) { return 123456; }

static int fails;
#define CHECK(name, cond) do { printf("{\"check\":\"%s\",\"ok\":%s}\n", name, (cond) ? "true" : "false"); fflush(stdout); if (!(cond)) fails++; } while (0)

static long fsize(const char *p) { struct stat st; return stat(p, &st) == 0 ? (long)st.st_size : -1; }

int main(int argc, char **argv)
{
    if (argc < 2 || chdir(argv[1]) != 0) return 2;
    mkdir("sdcard", 0777);
    evq_hil_io_init();

    /* C-33: parser matrix */
    const char *writers[] = { "evlog", "sdlog", "ambit_ota", "ambit_flash", "any" };
    const char *ops[] = { "open", "write", "read", "flush", "fsync", "close", "truncate", "rename", "remove", "mkdir", "stat", "any" };
    const char *modes[] = { "eio", "enospc", "short", "applied_eio", "reset_before", "reset_after", "reset_mid_write", "off" };
    unsigned valid = 0, invalid = 0, wrong = 0;
    for (size_t w = 0; w < 5; w++) for (size_t o = 0; o < 12; o++) for (size_t m = 0; m < 8; m++) {
        uint8_t ww, oo; evq_iom_t mm;
        int r = evq_arm_io_parse(writers[w], ops[o], modes[m], &ww, &oo, &mm);
        bool write_only = !strcmp(modes[m], "short") || !strcmp(modes[m], "reset_mid_write");
        bool applied = !strcmp(modes[m], "applied_eio");
        bool want = write_only ? !strcmp(ops[o], "write")
                  : applied ? (!strcmp(ops[o], "rename") || !strcmp(ops[o], "remove") || !strcmp(ops[o], "fsync") ||
                               !strcmp(ops[o], "close") || !strcmp(ops[o], "truncate"))
                  : true;
        if ((r == 0) != want) { wrong++; printf("{\"parse_mismatch\":\"%s %s %s\",\"r\":%d}\n", writers[w], ops[o], modes[m], r); }
        if (r == 0) valid++; else invalid++;
        if (r != 0 && r != -4) wrong++;
    }
    printf("{\"parse_valid\":%u,\"parse_invalid\":%u}\n", valid, invalid);
    CHECK("C-33 valid combos accepted, invalid refused (-4)", wrong == 0);
    uint8_t ww, oo; evq_iom_t mm;
    CHECK("C-33 unknown writer/op/mode refused", evq_arm_io_parse("sdlogg", "fsync", "eio", &ww, &oo, &mm) == -1 &&
          evq_arm_io_parse("sdlog", "fsyncc", "eio", &ww, &oo, &mm) == -2 &&
          evq_arm_io_parse("sdlog", "fsync", "eioo", &ww, &oo, &mm) == -3 &&
          evq_arm_io_parse("sdlog", "verify", "eio", &ww, &oo, &mm) == -2);

    /* C-34: nth / count on matching writer+op only */
    evq_arm_io_set(SD_DIAG_W_SDLOG, SD_DIAG_OP_FSYNC, EVQ_IOM_EIO, 3, 2);
    unsigned nth = 0;
    int seq[8];
    for (int i = 0; i < 8; i++) {
        (void)evq_arm_io_hit(SD_DIAG_W_EVLOG, SD_DIAG_OP_FSYNC, &nth);      /* other writer: never counts */
        (void)evq_arm_io_hit(SD_DIAG_W_SDLOG, SD_DIAG_OP_WRITE, &nth);      /* other op: never counts */
        seq[i] = evq_arm_io_hit(SD_DIAG_W_SDLOG, SD_DIAG_OP_FSYNC, &nth) == EVQ_IOM_EIO;
    }
    CHECK("C-34 nth=3 count=2 fires exactly on matches 3 and 4", !seq[0] && !seq[1] && seq[2] && seq[3] && !seq[4] && !seq[7]);

    /* C-34: short leaves floor(n/2) bytes and returns floor(n/2) with EIO */
    evq_arm_io_set(SD_DIAG_W_SDLOG, SD_DIAG_OP_WRITE, EVQ_IOM_SHORT, 1, 1);
    FILE *f = evq_hil_io_fopen_w(SD_DIAG_W_SDLOG, "sdcard/a.log", "w");
    char buf[101];
    memset(buf, 'x', sizeof buf);
    errno = 0;
    size_t got = evq_hil_io_fwrite_w(SD_DIAG_W_SDLOG, buf, 1, 101, f);
    int e = errno;
    evq_hil_io_fclose_w(SD_DIAG_W_SDLOG, f);
    CHECK("C-34 short: returns floor(n/2), errno EIO, floor(n/2) on disk", got == 50 && e == EIO && fsize("sdcard/a.log") == 50);

    /* C-34: applied_eio performs the op then reports EIO */
    f = evq_hil_io_fopen_w(SD_DIAG_W_EVLOG, "sdcard/b.tmp", "w");
    evq_hil_io_fclose_w(SD_DIAG_W_EVLOG, f);
    evq_arm_io_set(SD_DIAG_W_EVLOG, SD_DIAG_OP_RENAME, EVQ_IOM_APPLIED_EIO, 1, 1);
    errno = 0;
    int rr = evq_hil_io_rename_w(SD_DIAG_W_EVLOG, "sdcard/b.tmp", "sdcard/b.log");
    e = errno;
    CHECK("C-34 applied_eio: rename applied, reported -1/EIO", rr == -1 && e == EIO && fsize("sdcard/b.log") == 0 &&
          fsize("sdcard/b.tmp") == -1);

    /* C-35: CPU reset kinds are labelled truthfully and leave a retained record */
    evq_arm_io_set(SD_DIAG_W_AMBIT_OTA, SD_DIAG_OP_FSYNC, EVQ_IOM_RESET_BEFORE, 1, 1);
    f = evq_hil_io_fopen_w(SD_DIAG_W_AMBIT_OTA, "sdcard/c.bin", "w");
    int reset = 0;
    if (setjmp(s_reset) == 0) {
        (void)evq_hil_io_fsync_w(SD_DIAG_W_AMBIT_OTA, fileno(f));
    } else {
        reset = 1;                                         /* came back through the "CPU reset" */
    }
    CHECK("C-35 reset_before fired", reset == 1);
    s_reason = ESP_RST_SW;
    evq_hil_io_init();                                     /* next boot */
    evq_hil_last_view_t lv;
    bool have = evq_hil_fault_last(&lv);
    CHECK("C-35 retained last fault survives a CPU reset", have && lv.mode == EVQ_IOM_RESET_BEFORE &&
          lv.writer == SD_DIAG_W_AMBIT_OTA && lv.op == SD_DIAG_OP_FSYNC && strstr(lv.path, "c.bin"));
    CHECK("C-35 reset kind named cpu_reset", strcmp(evq_iom_kind((evq_iom_t)lv.mode), "cpu_reset") == 0);
    s_reason = ESP_RST_POWERON;
    evq_hil_io_init();
    CHECK("C-35 retained record invalid after power-on", !evq_hil_fault_last(&lv));

    /* FIO-2 (Sprint 2 H1): two coexisting slots + path filter. Built twice by
     * tests/test_sd_write_integrity.py: against the working tree (all pass) and,
     * with -DFIO_BASELINE_SEMANTICS, against the 4ec9cef trace/io sources whose
     * single slot is REPLACED by a second arm — the coexistence checks must fail
     * there, proving the regression discriminates. */
    evq_arm_io_clear();
    evq_iom_t hm;
    unsigned hn = 0;
    int hs = -1;
    CHECK("FIO-2 add refused without slot A", evq_arm_io_set_slot(1, SD_DIAG_W_SDLOG, SD_DIAG_OP_TRUNCATE,
                                                                   EVQ_IOM_EIO, 1, 1, NULL) != 0);
    (void)evq_arm_io_set_slot(0, SD_DIAG_W_SDLOG, SD_DIAG_OP_FSYNC, EVQ_IOM_EIO, 1, 1, NULL);
    int badd = evq_arm_io_set_slot(1, SD_DIAG_W_SDLOG, SD_DIAG_OP_TRUNCATE, EVQ_IOM_EIO, 1, 1, NULL);
    CHECK("FIO-2 slot B armed beside A", badd == 0);
    CHECK("FIO-2 add refused while B busy", evq_arm_io_set_slot(1, SD_DIAG_W_SDLOG, SD_DIAG_OP_WRITE,
                                                                 EVQ_IOM_EIO, 1, 1, NULL) != 0);
    /* The L-4b flow: commit fsync fails (A), then the rollback truncate fails (B). */
    f = evq_hil_io_fopen_w(SD_DIAG_W_SDLOG, "sdcard/q.log", "w");
    (void)evq_hil_io_fwrite_w(SD_DIAG_W_SDLOG, "abcdefgh\n", 1, 9, f);
    fflush(f);                                             /* bytes on "disk" before the failing ops */
    errno = 0;
    int fs = evq_hil_io_fsync_w(SD_DIAG_W_SDLOG, fileno(f));
    int fs_errno = errno;
    errno = 0;
    int tr = evq_hil_io_ftruncate_w(SD_DIAG_W_SDLOG, fileno(f), 0);
    int tr_errno = errno;
    CHECK("FIO-2 slot A fired on fsync", fs == -1 && fs_errno == EIO);
    CHECK("FIO-2 slot B fired on the following truncate (both coexist)", tr == -1 && tr_errno == EIO);
    CHECK("FIO-2 truncate not performed by injected slot B", fsize("sdcard/q.log") == 9);
    (void)evq_hil_io_fclose_w(SD_DIAG_W_SDLOG, f);
    /* Path filter: only renames whose path contains the substring count. */
    (void)evq_arm_io_set_slot(0, SD_DIAG_W_EVLOG, SD_DIAG_OP_RENAME, EVQ_IOM_EIO, 1, 1, "/evq/m-");
    hm = evq_arm_io_hit_p(SD_DIAG_W_EVLOG, SD_DIAG_OP_RENAME, "/sdcard/events/ev-000001.tmp", &hn, &hs);
    CHECK("FIO-2 path filter skips a non-matching path", hm == EVQ_IOM_NONE);
    hm = evq_arm_io_hit_p(SD_DIAG_W_EVLOG, SD_DIAG_OP_RENAME, "/sdcard/evq/m-000001.tmp", &hn, &hs);
    CHECK("FIO-2 path filter fires on the matching path as its 1st match", hm == EVQ_IOM_EIO && hn == 1 && hs == 0);
    /* Priority and independence: A and B both match one op -> only A fires. */
    (void)evq_arm_io_set_slot(0, SD_DIAG_W_SDLOG, SD_DIAG_OP_WRITE, EVQ_IOM_EIO, 1, 1, NULL);
    (void)evq_arm_io_set_slot(1, SD_DIAG_W_SDLOG, SD_DIAG_OP_WRITE, EVQ_IOM_ENOSPC, 1, 1, NULL);
    hm = evq_arm_io_hit_p(SD_DIAG_W_SDLOG, SD_DIAG_OP_WRITE, "sdcard/logs/ambyte.log", &hn, &hs);
    evq_iom_t hm2 = evq_arm_io_hit_p(SD_DIAG_W_SDLOG, SD_DIAG_OP_WRITE, "sdcard/logs/ambyte.log", &hn, &hs);
    CHECK("FIO-2 one slot per op, A first, then B on the next op", hm == EVQ_IOM_EIO && hm2 == EVQ_IOM_ENOSPC && hs == 1);
    /* Clearing: `fault io any any off` = clear both. */
    (void)evq_arm_io_set_slot(0, SD_DIAG_W_SDLOG, SD_DIAG_OP_WRITE, EVQ_IOM_EIO, 1, 1, NULL);
    (void)evq_arm_io_set_slot(1, SD_DIAG_W_SDLOG, SD_DIAG_OP_FSYNC, EVQ_IOM_EIO, 1, 1, NULL);
    evq_arm_io_clear();
    CHECK("FIO-2 clear disarms both slots",
          evq_arm_io_hit_p(SD_DIAG_W_SDLOG, SD_DIAG_OP_WRITE, "x", &hn, &hs) == EVQ_IOM_NONE &&
          evq_arm_io_hit_p(SD_DIAG_W_SDLOG, SD_DIAG_OP_FSYNC, "x", &hn, &hs) == EVQ_IOM_NONE);
    /* Named-point arm coexists with an I/O slot (independent state). */
    evq_arm_set("store.before_write", EVQ_ARM_EIO, 1);
    (void)evq_arm_io_set_slot(0, SD_DIAG_W_SDLOG, SD_DIAG_OP_WRITE, EVQ_IOM_EIO, 1, 1, NULL);
    char d1[160];
    CHECK("FIO-2 named point survives an I/O arm", evq_arm_describe(d1, sizeof d1) && strstr(d1, "store.before_write"));
    evq_arm_clear();
    evq_arm_io_clear();

    /* WIT-1 (H8a): a sdlog reset_mid_write prints the exact chunk + half and a
     * complete synchronous trace drain BEFORE the reset (checked byte-exact by
     * tests/test_sd_write_integrity.py from stdout). */
    s_reason = ESP_RST_SW;
    CHECK("WIT-1 trace on", hil_sdl_trace_on(false) == 0);
    static const uint8_t r1[] = "2026-09-27 10:00:00  W (1) HILSDLOG: wit 100000 aaaa\n";
    static const uint8_t r2[] = "2026-09-27 10:00:00  W (2) HILSDLOG: wit 100001 bbbb\n";
    hil_sdl_push(r1, sizeof r1 - 1, true);
    hil_sdl_push(r2, sizeof r2 - 1, true);
    hil_sdl_pop(sizeof r1 - 1 + sizeof r2 - 1);
    f = evq_hil_io_fopen_w(SD_DIAG_W_SDLOG, "sdcard/w.log", "a+");
    (void)evq_hil_io_fwrite_w(SD_DIAG_W_SDLOG, "prefix\n", 1, 7, f);
    fflush(f);
    evq_arm_io_set(SD_DIAG_W_SDLOG, SD_DIAG_OP_WRITE, EVQ_IOM_RESET_MID_WRITE, 1, 1);
    uint8_t chunk[sizeof r1 - 1 + sizeof r2 - 1];
    memcpy(chunk, r1, sizeof r1 - 1);
    memcpy(chunk + sizeof r1 - 1, r2, sizeof r2 - 1);
    printf("WIT1_CHUNK_LEN %u\n", (unsigned)sizeof chunk);
    int wreset = 0;
    if (setjmp(s_reset) == 0) {
        (void)evq_hil_io_fwrite_w(SD_DIAG_W_SDLOG, chunk, 1, sizeof chunk, f);
    } else {
        wreset = 1;
    }
    CHECK("WIT-1 reset happened", wreset == 1);
    CHECK("WIT-1 torn tail = prefix + half chunk on disk", fsize("sdcard/w.log") == 7 + (long)(sizeof chunk / 2));
    printf("{\"fails\":%d}\n", fails);
    return fails != 0;
}
