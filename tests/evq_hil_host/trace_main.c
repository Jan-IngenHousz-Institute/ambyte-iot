/* IO-1 host driver for components/event_log/evq_hil_trace.c: classification,
 * ring accounting (total/lost/first/last with contiguous seqs across drains)
 * and one-shot fault arming. Prints JSON lines the unittest checks. */
#include <stdio.h>
#include <string.h>

#include "evq_hil_trace.h"

static evq_tr_entry_t ring[EVQ_TR_CAP];
static unsigned long long emitted_first, emitted_last, emitted_n, prev_seq, gaps;

static void emit(const evq_tr_entry_t *e, void *ctx)
{
    (void)ctx;
    if (emitted_n == 0) emitted_first = e->seq;
    if (prev_seq && e->seq != prev_seq + 1) gaps++;
    prev_seq = e->seq;
    emitted_last = e->seq;
    emitted_n++;
}

static void drain(const char *label)
{
    unsigned long long lost = 0, first = 0, last = 0;
    emitted_n = 0;
    size_t n = evq_tr_drain(emit, NULL, (uint64_t *)&lost, (uint64_t *)&first, (uint64_t *)&last);
    printf("{\"drain\":\"%s\",\"n\":%zu,\"lost\":%llu,\"first\":%llu,\"last\":%llu,\"gaps\":%llu,\"total\":%llu}\n",
           label, n, lost, first, last, gaps, (unsigned long long)evq_tr_total());
}

int main(void)
{
    printf("{\"cls\":[%d,%d,%d,%d,%d,%d,%d]}\n",
           evq_tr_is_sd_record("/sdcard/events/ev-000001.log"), evq_tr_is_sd_record("/sdcard/evq/m-000002.log"),
           evq_tr_is_sd_record("/sdcard/archive/arc-5.log"), evq_tr_is_sd_record("/sdcard/logs/ambyte.log"),
           evq_tr_is_index("/evstore/evq.idx"), evq_tr_is_flash_segment("/evstore/events/ev-000007.log"),
           evq_tr_is_flash_segment("/evstore/events/quarantine.log"));
    evq_tr_init(ring);
    for (int i = 0; i < 100; i++) evq_tr_record(i, EVQ_TR_RENAME, 0, 0, "a", "b");
    drain("d1");                                    /* 100, lost 0 */
    for (unsigned i = 0; i < EVQ_TR_CAP + 50; i++) evq_tr_record(i, EVQ_TR_REMOVE, 0, 0, "x", NULL);
    drain("d2");                                    /* overflow: lost 50 */
    for (int i = 0; i < 10; i++) evq_tr_record(i, EVQ_TR_FSYNC, 0, 0, "y", NULL);
    drain("d3");                                    /* contiguous after d2 */
    drain("d4");                                    /* empty */
    evq_arm_set("spool.mid_copy", EVQ_ARM_EIO, 2);
    int a1 = evq_arm_hit("spool.mid_copy");         /* 1st hit: not yet */
    int io1 = evq_arm_take_io();
    int a2 = evq_arm_hit("spool.mid_copy");         /* 2nd hit: arms EIO */
    int io2 = evq_arm_take_io();
    int io3 = evq_arm_take_io();                    /* consumed */
    int a3 = evq_arm_hit("spool.mid_copy");         /* one shot */
    evq_arm_set("store.before_write", EVQ_ARM_RESET, 1);
    int r1 = evq_arm_hit("other.point");
    int r2 = evq_arm_hit("store.before_write");
    evq_arm_set("x", EVQ_ARM_RESET_INSIDE, 1);
    (void)evq_arm_hit("x");
    int ri = evq_arm_take_io();
    printf("{\"arm\":[%d,%d,%d,%d,%d,%d,%d,%d,%d]}\n", a1, io1, a2, io2, io3, a3, r1, r2, ri);
    return 0;
}
