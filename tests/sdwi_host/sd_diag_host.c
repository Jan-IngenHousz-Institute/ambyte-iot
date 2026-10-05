/* Host checks of the pure sd_diag core (contract C-27, C-28, C-32 render). */
#include <stdio.h>
#include <string.h>
#include "sd_diag.h"

static int fails;
#define CHECK(name, cond) do { printf("{\"check\":\"%s\",\"ok\":%s}\n", name, (cond) ? "true" : "false"); if (!(cond)) fails++; } while (0)

int main(void)
{
    sd_diag_block_t b, floor;
    memset(&b, 0xA5, sizeof b);                         /* garbage RTC after power-on */
    sd_diag_core_boot(&b, false);
    CHECK("poweron: provisional until floor merge", b.floor_pending == 1 && b.exact == 0);
    sd_diag_core_fault(&b, SD_DIAG_W_SDLOG, SD_DIAG_OP_FSYNC, 5, 10);   /* early fault before NVS */
    sd_diag_core_merge_floor(&b, NULL);
    CHECK("power-on without a floor is inexact epoch 1 (fail closed)", b.exact == 0 && b.epoch == 1 && b.boot_seq == 1);
    CHECK("early fault kept", b.cnt[SD_DIAG_W_SDLOG][SD_DIAG_OP_FSYNC] == 1);
    for (int i = 0; i < 3; i++) sd_diag_core_fault(&b, SD_DIAG_W_EVLOG, SD_DIAG_OP_RENAME, 5, 100 + i);
    sd_diag_core_refusal(&b, SD_DIAG_REF_FULL, 41, 28, 2, 1, 1790000000000LL, 200);
    sd_diag_core_refusal(&b, SD_DIAG_REF_FULL, 42, 28, 2, 1, 1790000001000LL, 201);
    floor = b;                                           /* NVS snapshot */
    sd_diag_core_fault(&b, SD_DIAG_W_EVLOG, SD_DIAG_OP_RENAME, 5, 300);  /* after snapshot */

    /* SW / panic / WDT reset: RTC retained, CRC valid */
    sd_diag_block_t cpu = b;
    sd_diag_core_boot(&cpu, true);
    sd_diag_core_merge_floor(&cpu, &floor);              /* no-op: not floor_pending */
    CHECK("cpu reset continues (exactness unchanged)", cpu.exact == b.exact && cpu.cnt[SD_DIAG_W_EVLOG][SD_DIAG_OP_RENAME] == 4 &&
          cpu.boot_seq == b.boot_seq + 1 && cpu.epoch == b.epoch);
    CHECK("refusal record kept across cpu reset", cpu.refused[SD_DIAG_REF_FULL] == 2 && cpu.ref_first_id == 41 &&
          cpu.ref_last_id == 42 && cpu.ref_last.err == 28);

    /* corrupt RTC (bad CRC) on a CPU reset → floor, inexact, new epoch */
    sd_diag_block_t bad = b;
    bad.cnt[0][0] ^= 1;                                  /* CRC no longer matches */
    sd_diag_core_boot(&bad, true);
    sd_diag_core_merge_floor(&bad, &floor);
    CHECK("bad crc -> floor, inexact, epoch+1", bad.exact == 0 && bad.epoch == floor.epoch + 1 &&
          bad.cnt[SD_DIAG_W_EVLOG][SD_DIAG_OP_RENAME] == 3);

    /* power-on with a floor: counts >= floor, inexact, epoch+1 */
    sd_diag_block_t po;
    memset(&po, 0, sizeof po);
    sd_diag_core_boot(&po, false);
    sd_diag_core_fault(&po, SD_DIAG_W_AMBIT_OTA, SD_DIAG_OP_WRITE, 5, 5);   /* before NVS */
    sd_diag_core_merge_floor(&po, &floor);
    CHECK("poweron with floor: inexact epoch+1", po.exact == 0 && po.epoch == floor.epoch + 1);
    CHECK("poweron counts = floor + early", po.cnt[SD_DIAG_W_EVLOG][SD_DIAG_OP_RENAME] == 3 &&
          po.cnt[SD_DIAG_W_AMBIT_OTA][SD_DIAG_OP_WRITE] == 1 && po.refused[SD_DIAG_REF_FULL] == 2);
    sd_diag_block_t po2;
    memset(&po2, 0, sizeof po2);
    sd_diag_core_boot(&po2, false);
    sd_diag_core_merge_floor(&po2, NULL);                /* missing/corrupt snapshot */
    CHECK("no readable snapshot: inexact", po2.exact == 0 && po2.epoch == 1);
    sd_diag_block_t exact_blk = po2;                     /* an exact block (only possible by construction) */
    exact_blk.exact = 1; sd_diag_seal(&exact_blk);
    sd_diag_core_boot(&exact_blk, true);
    CHECK("exact survives only a valid-CRC CPU reset", exact_blk.exact == 1);
    sd_diag_core_boot(&exact_blk, false);
    sd_diag_core_merge_floor(&exact_blk, &floor);
    CHECK("power-on always drops exactness even with a valid floor", exact_blk.exact == 0);

    /* generation continuity across a valid-floor merge (eval round 2):
     * floor gen 2 → merge → one new fault must persist (normal + forced), and
     * an unchanged block after a successful snapshot must be skipped. */
    {
        sd_diag_block_t f2;
        memset(&f2, 0, sizeof f2);
        sd_diag_core_boot(&f2, false);
        sd_diag_core_merge_floor(&f2, NULL);                       /* gen 1 */
        sd_diag_core_fault(&f2, SD_DIAG_W_SDLOG, SD_DIAG_OP_FSYNC, 5, 1);   /* gen 2 */
        uint32_t persisted = f2.gen;                               /* snapshot at gen 2 */
        sd_diag_block_t nb;
        memset(&nb, 0, sizeof nb);
        sd_diag_core_boot(&nb, false);
        sd_diag_core_merge_floor(&nb, &f2);
        CHECK("merged gen strictly above the floor's", nb.gen > persisted);
        sd_diag_core_fault(&nb, SD_DIAG_W_SDLOG, SD_DIAG_OP_WRITE, 5, 2);
        CHECK("new fault after floor merge persists (first change)",
              sd_diag_core_should_persist(&nb, persisted, false, 0, 10, false));
        CHECK("new fault after floor merge persists (forced)",
              sd_diag_core_should_persist(&nb, persisted, true, 10, 10, true));
        uint32_t after = nb.gen;                                   /* successful snapshot */
        CHECK("unchanged block after success is skipped (normal)",
              !sd_diag_core_should_persist(&nb, after, true, 10, 700000, false));
        CHECK("unchanged block after success is skipped (forced)",
              !sd_diag_core_should_persist(&nb, after, true, 10, 700000, true));
        sd_diag_block_t eb;                                        /* early pre-NVS change + floor */
        memset(&eb, 0, sizeof eb);
        sd_diag_core_boot(&eb, false);
        sd_diag_core_fault(&eb, SD_DIAG_W_SDLOG, SD_DIAG_OP_OPEN, 5, 1);
        sd_diag_core_merge_floor(&eb, &f2);
        CHECK("early pre-NVS change keeps gen above the floor", eb.gen > persisted);
    }

    /* snapshot policy: 1000 faults over one hour, heartbeat every 5 min */
    sd_diag_block_t p = b;
    uint32_t persisted_gen = p.gen, last = 0;
    bool this_boot = false;
    unsigned writes = 0, t = 0;
    for (int i = 0; i < 1000; i++) {
        t = (uint32_t)((uint64_t)i * 3600000u / 1000u);
        sd_diag_core_fault(&p, SD_DIAG_W_SDLOG, SD_DIAG_OP_WRITE, 5, t);
        if (t / 300000u != (t + 3600) / 300000u || i == 0) {   /* heartbeat tick */
            if (sd_diag_core_should_persist(&p, persisted_gen, this_boot, last, t, false)) {
                writes++; persisted_gen = p.gen; this_boot = true; last = t;
            }
        }
    }
    printf("{\"persist_writes\":%u}\n", writes);
    CHECK("<= 7 NVS writes for 1000 faults in 1 h", writes >= 1 && writes <= 7);
    CHECK("no write when unchanged even if forced", !sd_diag_core_should_persist(&p, p.gen, true, last, t, true));
    CHECK("forced write when changed", sd_diag_core_should_persist(&p, p.gen - 1, true, t, t, true));
    CHECK("never persist before the floor merge", ({ sd_diag_block_t q; memset(&q, 0, sizeof q);
          sd_diag_core_boot(&q, false); !sd_diag_core_should_persist(&q, 99, false, 0, 0, true); }));

    /* saturation, render */
    for (int i = 0; i < 70000; i++) sd_diag_core_fault(&p, SD_DIAG_W_AMBIT_FLASH, SD_DIAG_OP_MKDIR, 5, 1);
    CHECK("counters saturate, never wrap", p.cnt[SD_DIAG_W_AMBIT_FLASH][SD_DIAG_OP_MKDIR] == 0xFFFF);
    char j[2048];
    int n = sd_diag_render_json(&po, j, sizeof j);
    printf("{\"render\":%s}\n", n > 0 ? j : "null");
    CHECK("render includes exact and epoch", n > 0 && strstr(j, "\"exact\":false") && strstr(j, "\"epoch\":"));
    CHECK("render refuses to truncate", sd_diag_render_json(&po, j, 16) == -1);
    CHECK("block fits the 256-B RTC budget", sizeof(sd_diag_block_t) <= 256);
    printf("{\"fails\":%d}\n", fails);
    return fails != 0;
}
