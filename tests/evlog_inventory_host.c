/* Host harness for evlog_inventory: builds a synthetic /archive + /events tree in
 * a temp dir, runs the real scanner and renderer, prints KEY=VALUE lines and the
 * rendered JSON for tests/test_evlog_inventory.py to assert on. */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "evlog_inventory.h"

static int s_begin_calls, s_end_calls;
static bool hook_begin(void) { s_begin_calls++; return true; }
static void hook_end(void) { s_end_calls++; }

static void write_file(const char *path, const char *content)
{
    FILE *f = fopen(path, "wb");
    assert(f != NULL);
    assert(fwrite(content, 1, strlen(content), f) == strlen(content));
    fclose(f);
}

/* One v2 record: id, channel, device, tag, cmd_raw, start_ms, end_ms, metadata, payload. */
static void record(char *dst, size_t cap, long long id, const char *cmd, long long start_ms)
{
    snprintf(dst, cap, "%lld\tuart_0\t28:37:2F:FF:FC:80\tTRACE\t%s\t%lld\t%lld\t\t{\"schema\":\"ambit.trace/3\"}\n",
             id, cmd, start_ms, start_ms + 1000);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    const char *root = argv[1];
    char arch[512], legacy[512], path[600];
    snprintf(arch, sizeof arch, "%s/archive", root);
    snprintf(legacy, sizeof legacy, "%s/events", root);
    assert(mkdir(arch, 0777) == 0 || errno == EEXIST);
    assert(mkdir(legacy, 0777) == 0 || errno == EEXIST);

    /* arc-100.log: ids 100..104; 102 predates the clock floor (1970-era). */
    char buf[8192], line[2048];
    buf[0] = '\0';
    for (long long id = 100; id <= 104; id++) {
        record(line, sizeof line, id, "arrun", id == 102 ? 86400000LL : 1758000000000LL + id * 60000);
        strcat(buf, line);
    }
    snprintf(path, sizeof path, "%s/arc-100.log", arch);
    write_file(path, buf);

    /* arc-200.log: 200 and 201 normal; 202 carries a cmd_raw longer than the head
     * buffer (still a valid record, but its start_ms is beyond the head → unparsed);
     * then a torn trailing line without newline. */
    buf[0] = '\0';
    record(line, sizeof line, 200, "arrun", 1758100000000LL); strcat(buf, line);
    record(line, sizeof line, 201, "arrun", 1758100060000LL); strcat(buf, line);
    {
        static char longcmd[1500];
        memset(longcmd, 'x', sizeof longcmd - 1);
        longcmd[sizeof longcmd - 1] = '\0';
        record(line, sizeof line, 202, longcmd, 1758100120000LL); strcat(buf, line);
    }
    strcat(buf, "203\tuart_0\tdev\tTRACE\tarrun\t1758100180000\t");   /* torn */
    snprintf(path, sizeof path, "%s/arc-200.log", arch);
    write_file(path, buf);

    /* Distractors: a non-matching name and a hidden file. */
    snprintf(path, sizeof path, "%s/notes.txt", arch);
    write_file(path, "hello\n");
    snprintf(path, sizeof path, "%s/.hidden", arch);
    write_file(path, "x\n");
    /* Legacy import dir with two ev files and one stray. */
    snprintf(path, sizeof path, "%s/ev-000001.log", legacy); write_file(path, "1\ta\tb\tc\td\t1\t2\t\t{}\n");
    snprintf(path, sizeof path, "%s/ev-000002.log", legacy); write_file(path, "2\ta\tb\tc\td\t1\t2\t\t{}\n");
    snprintf(path, sizeof path, "%s/other.bin", legacy);     write_file(path, "zz");

    evlog_inventory_window_t win = { .from_id = 101, .to_id = 201, .from_ms = 0, .to_ms = 0 };
    evlog_inventory_hooks_t hooks = { .io_begin = hook_begin, .io_end = hook_end, .yield = NULL };
    evlog_inventory_t inv;
    esp_err_t err = evlog_inventory_scan(arch, legacy, &win, &hooks, &inv);
    assert(err == ESP_OK);
    assert(s_begin_calls == s_end_calls);           /* io gate is 1:1 */

    printf("present=%d\n", inv.archive_dir_present);
    printf("files=%u\n", inv.files);
    printf("other_entries=%u\n", inv.other_entries);
    printf("records=%u\n", inv.records);
    printf("torn=%u\n", inv.torn);
    printf("unparsed=%u\n", inv.unparsed);
    printf("in_window=%u\n", inv.in_window);
    printf("pre_2024=%u\n", inv.pre_clock_floor);
    printf("min_id=%lld\n", (long long)inv.min_id);
    printf("max_id=%lld\n", (long long)inv.max_id);
    printf("min_start_ms=%lld\n", (long long)inv.min_start_ms);
    printf("max_start_ms=%lld\n", (long long)inv.max_start_ms);
    printf("legacy_files=%u\n", inv.legacy_files);
    printf("listed=%u\n", inv.listed);
    printf("list0=%lld:%lld:%lld:%u:%u\n", (long long)inv.list[0].name_id, (long long)inv.list[0].first_id,
           (long long)inv.list[0].last_id, inv.list[0].records, inv.list[0].in_window);
    printf("list1=%lld:%lld:%lld:%u:%u\n", (long long)inv.list[1].name_id, (long long)inv.list[1].first_id,
           (long long)inv.list[1].last_id, inv.list[1].records, inv.list[1].in_window);
    printf("io_calls=%d\n", s_begin_calls);

    /* Renderer: escaping of a hostile id, fits in a field-sized buffer. */
    static char json[8192];
    int n = evlog_inventory_render_json(&inv, &win, "inv-1\"};alert(1)\\", "28:37:2F:FF:FC:80", "2.3.0",
                                        true, 2809856ULL, 135, 26466, 26470, 42, true, json, sizeof json);
    assert(n > 0 && n < (int)sizeof json);
    printf("JSON=%s\n", json);

    /* Renderer must refuse rather than truncate. */
    char tiny[64];
    assert(evlog_inventory_render_json(&inv, &win, "a", "b", "c", true, 0, 0, 0, 0, 0, false,
                                       tiny, sizeof tiny) == -1);

    /* Absent archive directory is not an error. */
    evlog_inventory_t none;
    snprintf(path, sizeof path, "%s/does-not-exist", root);
    assert(evlog_inventory_scan(path, NULL, NULL, NULL, &none) == ESP_OK);
    assert(!none.archive_dir_present && none.files == 0 && none.records == 0);
    printf("ABSENT_OK=1\n");
    return 0;
}
