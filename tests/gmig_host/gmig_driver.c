/* G-MIG driver: one "boot" of one firmware's PRODUCTION event_log against a
 * device state directory (CWD: evstore/, sdcard/, nvs.db, .shim/, out/).
 * Compiled twice by build.py: -DGMIG_V1110 against `git show v1.11.0:` event
 * store sources, and without it against the candidate's. Both builds share
 * the NVS model (nvs_model.c) and the tests/evq_host media shim/stubs, so the
 * two executables hand state to each other exactly like two images on one
 * device (same littlefs dir, same NVS).
 *
 *   gmig_driver <script>
 *
 * Script commands (one per line, '#' comments):
 *   store <n> <profile>      store n generated records (profile: bench|mixed|small)
 *   deliver <n|all> <log>    claim + in-order PUBACK (window <= 16) up to n; every
 *                            claimed record -> out/<log>.jsonl (id + sha256 of the
 *                            line re-rendered from the claimed fields)
 *   peek <n> <log>           claim up to n (<= window) WITHOUT ack, then revert all
 *   service <n>              n keeper passes (candidate: event_log_sd_service;
 *                            v1.11.0: app_main keeper body = legacy import + archive)
 *   sd insert|remove         mount / unmount the simulated card
 *   tick <ms>                advance the test clock
 *   health <label>           -> out/health.jsonl
 *   crash                    stop WITHOUT the shutdown handler (CPU reset / panic
 *                            / watchdog model: every byte already written stays)
 * Script end = graceful reboot (event_log_prepare_shutdown, as esp_restart's
 * shutdown handler does). Exit 0 = ok; anything else = harness/firmware error. */
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "event_log.h"
#include "evq_host.h"
#include "nvs.h"
#include "sd_card.h"
#include "sha256.h"

#ifdef GMIG_V1110
#define FW "v1.11.0"
#else
#define FW "candidate"
#endif

static FILE *s_acc, *s_hl, *s_ev;
static char  s_scenario[64] = "gmig";
static uint64_t s_seed = 1;
static uint64_t s_gen = 0;

void evq_host_flush_manifests(void)
{
    FILE *fs[] = { s_acc, s_hl, s_ev };
    for (size_t i = 0; i < sizeof fs / sizeof fs[0]; i++) if (fs[i]) fflush(fs[i]);
}

static FILE *open_out(const char *name)
{
    char p[160];
    snprintf(p, sizeof p, "out/%s", name);
    FILE *f = fopen(p, "a");
    if (f == NULL) { perror(p); exit(3); }
    return f;
}

static void load_gen(void)
{
    FILE *f = fopen("out/gen", "r");
    if (f) { if (fscanf(f, "%" SCNu64, &s_gen) != 1) s_gen = 0; fclose(f); }
}
static void save_gen(void)
{
    FILE *f = fopen("out/gen", "w");
    if (f) { fprintf(f, "%" PRIu64 "\n", s_gen); fclose(f); }
}

/* ── deterministic record generator ──────────────────────────────────────── */
static uint64_t mix(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

/* Payload sizes. bench: the bench device's shape (309 records fit ONE 256 KiB
 * file) with a multi-KB tail; mixed: the evq_host distribution up to the
 * 63 KB binding cap (forces rotations); small: 200-600 B. */
static size_t pick_size(uint64_t r, const char *profile)
{
    uint64_t r2 = mix(r);
    unsigned cls = (unsigned)(r % 100);
    if (strcmp(profile, "small") == 0) return 200 + (size_t)(r2 % 401);
    if (strcmp(profile, "bench") == 0) {
        if (cls < 85) return 250 + (size_t)(r2 % 501);            /* 250-750 B */
        if (cls < 97) return 750 + (size_t)(r2 % 751);            /* 750-1500 B */
        return 2048 + (size_t)(r2 % (5000 - 2048 + 1));           /* 2-5 KB */
    }
    if (cls < 70) return 200 + (size_t)(r2 % 1849);
    if (cls < 95) return 2048 + (size_t)(r2 % (16384 - 2048 + 1));
    return 61440 + (size_t)(r2 % (63000 - 61440 + 1));
}

typedef struct { char channel[12], device[24], tag[16]; char *cmd, *meta, *payload; int64_t s, e; } rec_t;

static void gen_record(uint64_t k, const char *profile, rec_t *r)
{
    uint64_t h = mix(s_seed * 1000003ULL + k);
    memset(r, 0, sizeof *r);
    snprintf(r->channel, sizeof r->channel, "uart_%u", (unsigned)(k % 4));
    snprintf(r->device, sizeof r->device, "28:37:2F:FF:E7:04");
    snprintf(r->tag, sizeof r->tag, (mix(h ^ 7) % 10) == 0 ? "STATUS" : "MEASUREMENT");
    size_t clen = (size_t)(mix(h ^ 1) % 121);
    r->cmd = malloc(clen + 1);
    for (size_t i = 0; i < clen; i++) r->cmd[i] = (char)('a' + (mix(h + i) % 26));
    if (clen > 5) memcpy(r->cmd, "arrun", 5);
    r->cmd[clen] = '\0';
    r->meta = malloc(80);
    if ((mix(h ^ 2) % 3) == 0) snprintf(r->meta, 80, "{\"k\":%" PRIu64 ",\"cal\":\"%08x\"}", k, (unsigned)(h & 0xffffffffu));
    else r->meta[0] = '\0';
    size_t target = pick_size(mix(h ^ 3), profile);
    r->payload = malloc(target + 256);
    int n = snprintf(r->payload, target + 256,
                     "{\"synthetic_test\":true,\"gmig\":\"%s\",\"seed\":%" PRIu64 ",\"k\":%" PRIu64 ",\"pad\":\"",
                     s_scenario, s_seed, k);
    size_t len = (size_t)n;
    while (len + 2 < target) { r->payload[len] = (char)('A' + (mix(h + len) % 26)); len++; }
    r->payload[len++] = '"';
    r->payload[len++] = '}';
    r->payload[len] = '\0';
    r->s = 1758000000000LL + (int64_t)k * 60000 + (int64_t)(mix(h ^ 4) % 30000);
    r->e = r->s + 1000 + (int64_t)(mix(h ^ 5) % 9000);
}

static void rec_free(rec_t *r) { free(r->cmd); free(r->meta); free(r->payload); }

/* The stored line exactly as every event_log revision in scope formats it. */
static char *render(int64_t id, const char *ch, const char *dev, const char *tag, const char *cmd,
                    int64_t s, int64_t e, const char *meta, const char *payload, size_t *out_len)
{
    size_t cap = strlen(cmd) + strlen(meta) + strlen(payload) + 256;
    char *b = malloc(cap);
    int n = snprintf(b, cap, "%lld\t%s\t%s\t%s\t%s\t%lld\t%lld\t%s\t%s\n", (long long)id, ch, dev, tag, cmd,
                     (long long)s, (long long)e, meta, payload);
    *out_len = (size_t)n;
    return b;
}

static void do_store(unsigned n, const char *profile)
{
    for (unsigned i = 0; i < n; i++) {
        uint64_t k = s_gen++;
        save_gen();
        rec_t r;
        gen_record(k, profile, &r);
        int64_t id = 0;
        if (event_log_next_id(&id) != ESP_OK) { fprintf(stderr, "next_id failed\n"); exit(4); }
        measurement_event_desc_t d = {
            .measure_id = id, .channel = r.channel, .device = r.device, .tag = r.tag, .cmd_raw = r.cmd,
            .start_ms = r.s, .end_ms = r.e, .metadata_json = r.meta, .payload_json = r.payload,
        };
        esp_err_t err = event_log_store_event(&d);
        size_t ll = 0;
        char *line = render(id, r.channel, r.device, r.tag, r.cmd, r.s, r.e, r.meta, r.payload, &ll);
        char hx[65];
        sha256_hex(line, ll, hx);
        free(line);
        fprintf(s_acc, "{\"fw\":\"%s\",\"k\":%" PRIu64 ",\"id\":%lld,\"rc\":%d,\"len\":%zu,\"line_sha\":\"%s\"}\n",
                FW, k, (long long)id, err, ll, hx);
        fflush(s_acc);
        rec_free(&r);
    }
}

static void log_event(FILE *f, const char *kind, const measurement_event_t *e, unsigned n)
{
    size_t ll = 0;
    char *line = render(e->measure_id, e->channel, e->device, e->tag, e->cmd_raw ? e->cmd_raw : "",
                        e->start_ticks_ms, e->end_ticks_ms, e->metadata_json ? e->metadata_json : "",
                        e->payload_json ? e->payload_json : "", &ll);
    char hx[65];
    sha256_hex(line, ll, hx);
    free(line);
    fprintf(f, "{\"fw\":\"%s\",\"kind\":\"%s\",\"n\":%u,\"id\":%lld,\"len\":%zu,\"line_sha\":\"%s\"}\n",
            FW, kind, n, (long long)e->measure_id, ll, hx);
    fflush(f);
}

static void ev(const char *fmt_json)
{
    fprintf(s_ev, "%s\n", fmt_json);
    fflush(s_ev);
}

/* Claim + in-order PUBACK. Returns the claim code that ended the run. */
static esp_err_t do_deliver(uint64_t max, const char *logname)
{
    FILE *lf = open_out(logname);
    int64_t win[16];
    uint64_t done = 0;
    esp_err_t last = ESP_OK;
    unsigned n_total = 0;
    for (;;) {
        unsigned nw = 0;
        while (nw < PUBLISH_WINDOW_SLOTS && (max == 0 || done + nw < max)) {
            measurement_event_t e;
            last = event_log_claim_next_event(&e);
            if (last != ESP_OK) break;
            log_event(lf, "claimed", &e, n_total++);
            win[nw++] = e.measure_id;
            measurement_event_free(&e);
        }
        if (nw == 0) break;
        for (unsigned i = 0; i < nw; i++) {
            esp_err_t m = event_log_mark_event_synced(win[i]);
            if (m != ESP_OK) { fprintf(stderr, "mark_synced(%lld) -> %d\n", (long long)win[i], m); exit(5); }
        }
        done += nw;
        if (max != 0 && done >= max) { last = ESP_OK; break; }
    }
    fclose(lf);
    char j[200];
    snprintf(j, sizeof j, "{\"fw\":\"%s\",\"cmd\":\"deliver\",\"log\":\"%s\",\"delivered\":%" PRIu64 ",\"end_code\":%d,\"end_name\":\"%s\"}",
             FW, logname, done, last, esp_err_to_name(last));
    ev(j);
    return last;
}

static void do_peek(unsigned n, const char *logname)
{
    FILE *lf = open_out(logname);
    int64_t win[16];
    unsigned nw = 0;
    esp_err_t last = ESP_OK;
    while (nw < n && nw < PUBLISH_WINDOW_SLOTS) {
        measurement_event_t e;
        last = event_log_claim_next_event(&e);
        if (last != ESP_OK) break;
        log_event(lf, "peeked", &e, nw);
        win[nw++] = e.measure_id;
        measurement_event_free(&e);
    }
    for (unsigned i = 0; i < nw; i++) {
        esp_err_t m = event_log_mark_event_pending(win[i]);
        if (m != ESP_OK) { fprintf(stderr, "mark_pending(%lld) -> %d\n", (long long)win[i], m); exit(5); }
    }
    /* A reverted window replays FIFO: the next claim must be the first peeked id. */
    if (nw > 0) {
        measurement_event_t e;
        esp_err_t rc = event_log_claim_next_event(&e);
        if (rc != ESP_OK || e.measure_id != win[0]) {
            fprintf(stderr, "FATAL: re-claim after revert returned rc=%d id=%lld, expected %lld\n", rc,
                    (long long)(rc == ESP_OK ? e.measure_id : -1), (long long)win[0]);
            exit(5);
        }
        (void)event_log_mark_event_pending(e.measure_id);
        measurement_event_free(&e);
    }
    fclose(lf);
    char j[200];
    snprintf(j, sizeof j, "{\"fw\":\"%s\",\"cmd\":\"peek\",\"log\":\"%s\",\"peeked\":%u,\"end_code\":%d}", FW, logname, nw, last);
    ev(j);
}

static void keeper_pass(void)
{
#ifdef GMIG_V1110
    /* v1.11.0 app_sd_keeper_task body (main/app_main.c at the tag). */
    if (sdcard_is_mounted()) {
        (void)event_log_import_sd_backlog(4);
        if (event_log_archive_pending()) { size_t a = 0; (void)event_log_archive_to_sd(&a); }
    }
#else
    (void)event_log_sd_service();
#endif
}

static void health(const char *label)
{
    evlog_health_t h;
    esp_err_t err = event_log_health(&h);
    uint32_t rs = 0, ro = 0, ts = 0;
    event_log_cursor_info(&rs, &ro, &ts);
    if (err != ESP_OK) { fprintf(s_hl, "{\"fw\":\"%s\",\"label\":\"%s\",\"err\":%d}\n", FW, label, err); fflush(s_hl); return; }
#ifdef GMIG_V1110
    fprintf(s_hl, "{\"fw\":\"%s\",\"label\":\"%s\",\"available\":%d,\"write_full\":%d,\"pending\":%lld,\"next_id\":%lld,"
                  "\"last_acked_id\":%lld,\"skipped\":%lld,\"dropped\":%lld,\"rd_seq\":%u,\"rd_off\":%u,\"tail_seq\":%u,"
                  "\"nvs_type_conflicts\":%u}\n",
            FW, label, h.available, h.write_full, (long long)h.pending, (long long)h.next_id, (long long)h.last_acked_id,
            (long long)h.skipped, (long long)h.dropped, rs, ro, ts, gmig_nvs_type_conflicts());
#else
    fprintf(s_hl, "{\"fw\":\"%s\",\"label\":\"%s\",\"available\":%d,\"write_full\":%d,\"pending\":%lld,\"pending_exact\":%d,"
                  "\"flash_pending\":%lld,\"sd_pending\":%lld,\"reimport_pending\":%lld,\"next_id\":%lld,"
                  "\"last_acked_id\":%lld,\"skipped\":%lld,\"dropped\":%lld,\"quarantined_poison\":%lld,"
                  "\"quarantined_malformed\":%lld,\"skipped_unindexed_gap\":%lld,\"corrupt_detected\":%lld,"
                  "\"rd_seq\":%u,\"rd_off\":%u,\"tail_seq\":%u,\"sd_state\":\"%s\",\"head_block\":\"%s\","
                  "\"index_segments\":%u,\"spool_files\":%u,\"reclaimed\":%u,\"archived\":%u,\"reimported\":%u,"
                  "\"nvs_type_conflicts\":%u}\n",
            FW, label, h.available, h.write_full, (long long)h.pending, h.pending_exact, (long long)h.flash_pending,
            (long long)h.sd_pending, (long long)h.reimport_pending, (long long)h.next_id, (long long)h.last_acked_id,
            (long long)h.skipped, (long long)h.dropped, (long long)h.quarantined_poison,
            (long long)h.quarantined_malformed, (long long)h.skipped_unindexed_gap, (long long)h.corrupt_detected,
            rs, ro, ts, event_log_sd_state_name(h.sd_state), event_log_block_name(h.head_block), h.index_segments,
            h.spool_files, h.reclaimed_files, h.archived_files, h.reimported_files, gmig_nvs_type_conflicts());
#endif
    fflush(s_hl);
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: gmig_driver <script>\n"); return 2; }
    const char *sc = getenv("GMIG_SCENARIO");
    if (sc) snprintf(s_scenario, sizeof s_scenario, "%s", sc);
    const char *sd = getenv("GMIG_SEED");
    if (sd) s_seed = strtoull(sd, NULL, 10);
    shim_seed(s_seed);
    mkdir("out", 0777);
    mkdir("evstore", 0777);
    s_acc = open_out("accepted.jsonl");
    s_hl = open_out("health.jsonl");
    s_ev = open_out("events.jsonl");
    load_gen();
    shim_mark("variant " FW);

    if (event_log_init() != ESP_OK) { fprintf(stderr, "event_log_init failed\n"); return 3; }
    health("boot");

    FILE *sf = fopen(argv[1], "r");
    if (sf == NULL) { perror(argv[1]); return 2; }
    char line[512];
    while (fgets(line, sizeof line, sf) != NULL) {
        char *nl = strchr(line, '\n'); if (nl) *nl = '\0';
        char *cmd = strtok(line, " ");
        if (cmd == NULL || cmd[0] == '#') continue;
        char *a1 = strtok(NULL, " "), *a2 = strtok(NULL, " ");
        if (strcmp(cmd, "store") == 0) do_store((unsigned)atoi(a1), a2 ? a2 : "bench");
        else if (strcmp(cmd, "deliver") == 0)
            do_deliver((a1 && strcmp(a1, "all") != 0) ? strtoull(a1, NULL, 10) : 0, a2 ? a2 : "delivered.jsonl");
        else if (strcmp(cmd, "peek") == 0) do_peek(a1 ? (unsigned)atoi(a1) : 16, a2 ? a2 : "peek.jsonl");
        else if (strcmp(cmd, "service") == 0) { int n = a1 ? atoi(a1) : 1; for (int i = 0; i < n; i++) keeper_pass(); }
        else if (strcmp(cmd, "sd") == 0) {
            uint32_t cid = evq_sd_stub_cid();
            if (a1 && strcmp(a1, "insert") == 0) { mkdir("sdcard", 0777); evq_sd_stub_set(true, cid); }
            else if (a1 && strcmp(a1, "remove") == 0) evq_sd_stub_set(false, cid);
            else { fprintf(stderr, "bad sd command\n"); return 2; }
#ifndef GMIG_V1110
            event_log_sd_notify();
#endif
        }
        else if (strcmp(cmd, "tick") == 0) evq_clock_advance((uint32_t)atoi(a1));
        else if (strcmp(cmd, "health") == 0) health(a1 ? a1 : "h");
        else if (strcmp(cmd, "crash") == 0) {
            health("pre-crash");
            evq_host_flush_manifests();
            _exit(0);                       /* no shutdown handler: CPU reset model */
        }
        else { fprintf(stderr, "unknown command %s\n", cmd); return 2; }
    }
    fclose(sf);
    health("pre-shutdown");
    event_log_prepare_shutdown();
    evq_host_flush_manifests();
    if (evq_sd_stub_refs() != 0) { fprintf(stderr, "FATAL: SD refs leaked: %d\n", evq_sd_stub_refs()); return 9; }
    if (gmig_nvs_type_conflicts() != 0) { fprintf(stderr, "FATAL: NVS type conflict(s)\n"); return 10; }
    return 0;
}
