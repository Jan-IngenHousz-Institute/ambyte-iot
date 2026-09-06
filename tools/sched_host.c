/*
 * sched_host.c — host CLI for the schedule spec, built from the same sources
 * the device compiles (plus time_sync.c and a fixed-offset localize stub), so
 * "passes CI" and "parses on the device" are the same check by construction.
 *
 *   sched_host --check <file>           compile; print errors, exit non-zero
 *   sched_host --schema                 action table as JSON Schema draft-07
 *   sched_host --simulate <file> --lat <deg> --lon <deg> --tz-offset-s <s>
 *                       --date YYYY-MM-DD [--days N]
 *                                       print the firing table per job
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sched_spec.h"

/* The due model's time base is local unix seconds; the host feeds local
 * times in directly, so localization is the identity. time_sync holds the
 * site lat/lon/offset for the sun math (set from --lat/--lon/--tz-offset-s). */
static int64_t local_identity(void *ctx, int64_t utc_unix)
{
    (void)ctx;
    return utc_unix;
}

static char *read_file(const char *path, size_t *len_out)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0 || n > SCHED_YAML_MAX_FILE_BYTES + 1) {
        fclose(f);
        return NULL;
    }
    char *buf = malloc((size_t)n + 1);
    if (buf == NULL) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
    fclose(f);
    buf[n] = '\0';
    *len_out = (size_t)n;
    return buf;
}

static int cmd_check(const char *path, int quiet)
{
    size_t len;
    char *text = read_file(path, &len);
    if (text == NULL) {
        fprintf(stderr, "%s: cannot read file (or exceeds %d bytes)\n",
                path, SCHED_YAML_MAX_FILE_BYTES);
        return 1;
    }
    static sched_program_t prog; /* static: ~17 KiB, keep it off the stack */
    char err[256];
    if (sched_compile_text(text, len, &prog, err, sizeof(err)) != ESP_OK) {
        fprintf(stderr, "%s:%s\n", path, err);
        free(text);
        return 1;
    }
    if (!quiet) {
        printf("OK %s: %d job(s), %d protocol(s), %d B pool, %d entries\n",
               sched_pool_str(&prog, prog.name_off) != NULL
                   ? sched_pool_str(&prog, prog.name_off) : "(unnamed)",
               prog.job_count, prog.protocol_count, prog.pool_used, prog.entry_count);
    }
    free(text);
    return 0;
}

static void fmt_hhmm(int64_t local, char *out, size_t cap)
{
    int h, m, s;
    time_sync_localtime(local, NULL, NULL, NULL, &h, &m, &s, NULL);
    if (s != 0) snprintf(out, cap, "%02d:%02d:%02d", h, m, s);
    else        snprintf(out, cap, "%02d:%02d", h, m);
}

typedef struct {
    int64_t *t;
    size_t   n, cap;
    bool     boot_fired;
    uint32_t skipped;
} job_fires_t;

static void record(job_fires_t *jf, int64_t t)
{
    if (jf->n == jf->cap) {
        jf->cap = jf->cap == 0 ? 64 : jf->cap * 2;
        jf->t = realloc(jf->t, jf->cap * sizeof(int64_t));
        if (jf->t == NULL) { fprintf(stderr, "out of memory\n"); exit(2); }
    }
    jf->t[jf->n++] = t;
}

static void print_fires(const char *name, const job_fires_t *jf)
{
    printf("%-16s %4zu fire(s)", name, jf->n + (jf->boot_fired ? 1 : 0));
    if (jf->boot_fired) printf(" boot");
    if (jf->n > 0) {
        /* uniform grid → compact "first..last every Ns"; else list */
        bool uniform = jf->n >= 2;
        for (size_t i = 2; uniform && i < jf->n; i++) {
            if (jf->t[i] - jf->t[i - 1] != jf->t[1] - jf->t[0]) uniform = false;
        }
        char a[16], b[16];
        if (uniform && jf->n > 8) {
            fmt_hhmm(jf->t[0], a, sizeof(a));
            fmt_hhmm(jf->t[jf->n - 1], b, sizeof(b));
            printf("  %s..%s every %llds", a, b, (long long)(jf->t[1] - jf->t[0]));
        } else {
            size_t show = jf->n <= 20 ? jf->n : 20;
            for (size_t i = 0; i < show; i++) {
                fmt_hhmm(jf->t[i], a, sizeof(a));
                printf(" %s", a);
            }
            if (jf->n > show) printf(" …");
        }
    }
    if (jf->skipped > 0) printf("  [%u skipped]", jf->skipped);
    printf("\n");
}

static int cmd_simulate(const char *path, double lat, double lon,
                        int32_t tz_off_s, const char *date, int days)
{
    int y, m, d;
    if (sscanf(date, "%d-%d-%d", &y, &m, &d) != 3 || m < 1 || m > 12 || d < 1 || d > 31) {
        fprintf(stderr, "--date must be YYYY-MM-DD\n");
        return 1;
    }
    size_t len;
    char *text = read_file(path, &len);
    if (text == NULL) {
        fprintf(stderr, "%s: cannot read file\n", path);
        return 1;
    }
    static sched_program_t prog;
    char err[256];
    if (sched_compile_text(text, len, &prog, err, sizeof(err)) != ESP_OK) {
        fprintf(stderr, "%s:%s\n", path, err);
        free(text);
        return 1;
    }
    time_sync_set_location(lat, lon, 0);
    time_sync_set_utc_offset_seconds(tz_off_s);

    int64_t start = time_sync_make(y, m, d, 0, 0, 0);
    int64_t end = start + (int64_t)days * 86400;

    printf("# simulate %s\n", path);
    printf("# schema %s", SCHED_SPEC_SCHEMA_CONST);
    const char *pid = sched_pool_str(&prog, prog.id_off);
    const char *pver = sched_pool_str(&prog, prog.version_off);
    if (pid != NULL) printf("  id %s", pid);
    if (pver != NULL) printf("  version %s", pver);
    printf("\n# site %.3f N %.3f E, tz offset %+d s\n", lat, lon, tz_off_s);
    int64_t rise, set;
    if (time_sync_sun_on_date(start + 43200, TIME_SYNC_SUNRISE, &rise) == ESP_OK &&
        time_sync_sun_on_date(start + 43200, TIME_SYNC_SUNSET, &set) == ESP_OK) {
        char a[16], b[16];
        fmt_hhmm(rise, a, sizeof(a));
        fmt_hhmm(set, b, sizeof(b));
        printf("# %s: sunrise %s local, sunset %s local\n", date, a, b);
    } else {
        printf("# %s: no sunrise/sunset (polar day or night)\n", date);
    }

    static sched_due_t due;
    sched_due_init(&due, &prog, local_identity, NULL, start);
    job_fires_t *fires = calloc((size_t)prog.job_count, sizeof(job_fires_t));
    if (fires == NULL) { free(text); return 2; }

    /* boot firings happen at runner start (after clock trust) */
    uint32_t mask = sched_due_poll(&due, start);
    for (int j = 0; j < prog.job_count; j++) {
        if (mask & (1u << j)) fires[j].boot_fired = true;
    }
    int64_t t = start;
    while (t < end) {
        int64_t next = sched_due_next(&due, t);
        if (next < 0 || next >= end) break;
        mask = sched_due_poll(&due, next);
        if (next <= t && mask == 0) { t = next + 1; continue; } /* no progress guard */
        t = next;
        for (int j = 0; j < prog.job_count; j++) {
            if (mask & (1u << j)) record(&fires[j], next);
        }
    }
    for (int j = 0; j < prog.job_count; j++) {
        fires[j].skipped = due.jobs[j].skipped;
        print_fires(sched_pool_str(&prog, prog.jobs[j].name_off), &fires[j]);
        free(fires[j].t);
    }
    free(fires);
    free(text);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 3 && strcmp(argv[1], "--check") == 0) {
        return cmd_check(argv[2], 0);
    }
    if (argc >= 2 && strcmp(argv[1], "--schema") == 0) {
        static char buf[8192];
        size_t n = sched_actions_dump_json(buf, sizeof(buf));
        if (n >= sizeof(buf)) {
            fprintf(stderr, "schema does not fit %zu bytes\n", sizeof(buf));
            return 2;
        }
        puts(buf);
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "--simulate") == 0) {
        const char *file = argv[2];
        double lat = 52.173, lon = 5.819; /* firmware compiled default (NL) */
        int32_t tz = 7200;                /* CEST summer default */
        const char *date = NULL;
        int days = 1;
        for (int i = 3; i + 1 < argc; i += 2) {
            if (strcmp(argv[i], "--lat") == 0) lat = strtod(argv[i + 1], NULL);
            else if (strcmp(argv[i], "--lon") == 0) lon = strtod(argv[i + 1], NULL);
            else if (strcmp(argv[i], "--tz-offset-s") == 0) tz = (int32_t)strtol(argv[i + 1], NULL, 10);
            else if (strcmp(argv[i], "--date") == 0) date = argv[i + 1];
            else if (strcmp(argv[i], "--days") == 0) days = atoi(argv[i + 1]);
            else {
                fprintf(stderr, "unknown option %s\n", argv[i]);
                return 2;
            }
        }
        if (date == NULL || days < 1) {
            fprintf(stderr, "--simulate needs --date YYYY-MM-DD [--days N≥1]\n");
            return 2;
        }
        return cmd_simulate(file, lat, lon, tz, date, days);
    }
    fprintf(stderr,
            "usage: sched_host --check <file>\n"
            "       sched_host --schema\n"
            "       sched_host --simulate <file> --date YYYY-MM-DD [--lat D] [--lon D]\n"
            "              [--tz-offset-s N] [--days N]\n");
    return 2;
}
