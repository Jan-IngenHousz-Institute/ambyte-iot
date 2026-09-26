/* Storage-harness driver: runs a scenario script against the PRODUCTION event
 * store (HEAD, or a baseline revision with -DEVQ_BASELINE) through its public
 * persistence port, with a broker simulator on the delivery side.
 *
 *   evq_driver <script> <start_line>
 *
 * CWD is the device state directory (evstore/, sdcard/, nvs.txt, .shim/, out/).
 * Exit codes: 0 script done, 85 graceful reboot requested, 86 simulated power
 * loss (the parent applies the power transform), anything else = harness error.
 * Manifests (out/NAME.jsonl) are written with plain libc and flushed per line so
 * a simulated power loss never loses harness evidence. */
#include <errno.h>
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
#include "sha256.h"

static FILE *s_acc, *s_ref, *s_att, *s_del, *s_hl, *s_clm;
static char  s_scenario[64] = "unnamed";
static uint64_t s_seed = 1;
static uint64_t s_gen = 0;         /* records generated so far (persisted) */
static uint64_t s_calls = 0;
static bool s_keeper_auto = true;
static int  s_boot = 0;
#ifdef EVQ_BASELINE
static unsigned s_stores_since_keeper = 0;
#endif

void evq_host_flush_manifests(void)
{
    FILE *fs[] = { s_acc, s_ref, s_att, s_del, s_hl, s_clm };
    for (size_t i = 0; i < sizeof fs / sizeof fs[0]; i++) if (fs[i]) fflush(fs[i]);
}

static FILE *open_out(const char *name)
{
    char p[128];
    snprintf(p, sizeof p, "out/%s", name);
    FILE *f = fopen(p, "a");
    if (f == NULL) { perror(p); exit(3); }
    return f;
}

static void save_gen(void)
{
    FILE *f = fopen("out/gen", "w");
    if (f) { fprintf(f, "%" PRIu64 " %" PRIu64 "\n", s_gen, s_calls); fclose(f); }
}

static void load_gen(void)
{
    FILE *f = fopen("out/gen", "r");
    if (f) { if (fscanf(f, "%" SCNu64 " %" SCNu64, &s_gen, &s_calls) != 2) { s_gen = 0; s_calls = 0; } fclose(f); }
}

/* ── deterministic record generator ──────────────────────────────────────── */
static uint64_t mix(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

typedef struct {
    char channel[12], device[24], tag[16];
    char *cmd, *meta, *payload;
    int64_t start_ms, end_ms;
} rec_t;

static size_t pick_size(uint64_t r, const char *profile)
{
    if (strcmp(profile, "small") == 0) return 200 + (size_t)(r % 401);          /* P-small: 200–600 B */
    if (strcmp(profile, "tiny") == 0) return 120 + (size_t)(r % 80);
    unsigned cls = (unsigned)(r % 100);
    uint64_t r2 = mix(r);
    if (cls < 70) return 200 + (size_t)(r2 % 1849);                              /* 200 B – 2 KiB */
    if (cls < 95) return 2048 + (size_t)(r2 % (16384 - 2048 + 1));               /* 2 – 16 KiB */
    return 61440 + (size_t)(r2 % (63000 - 61440 + 1));                           /* 60 KiB .. binding cap */
}

static void gen_record(uint64_t k, const char *profile, rec_t *r)
{
    uint64_t h = mix(s_seed * 1000003ULL + k);
    memset(r, 0, sizeof *r);
    snprintf(r->channel, sizeof r->channel, "uart_%u", (unsigned)(k % 4));
    snprintf(r->device, sizeof r->device, "28:37:2F:FF:E7:04");
    snprintf(r->tag, sizeof r->tag, "MEASUREMENT");
    size_t clen = (size_t)(mix(h ^ 1) % 541);
    r->cmd = malloc(clen + 1);
    for (size_t i = 0; i < clen; i++) r->cmd[i] = (char)('a' + (mix(h + i) % 26));
    if (clen > 5) memcpy(r->cmd, "arrun", 5);
    r->cmd[clen] = '\0';
    bool meta = (mix(h ^ 2) % 3) == 0;
    r->meta = malloc(64);
    if (meta) snprintf(r->meta, 64, "{\"k\":%" PRIu64 ",\"cal\":\"%08x\"}", k, (unsigned)(h & 0xffffffffu));
    else r->meta[0] = '\0';
    size_t target = pick_size(mix(h ^ 3), profile);
    r->payload = malloc(target + 256);
    int n = snprintf(r->payload, target + 256,
                     "{\"synthetic_test\":true,\"scenario\":\"%s\",\"seed\":%" PRIu64 ",\"k\":%" PRIu64 ",\"pad\":\"",
                     s_scenario, s_seed, k);
    size_t len = (size_t)n;
    while (len + 2 < target) { r->payload[len] = (char)('A' + (mix(h + len) % 26)); len++; }
    r->payload[len++] = '"';
    r->payload[len++] = '}';
    r->payload[len] = '\0';
    r->start_ms = 1758000000000LL + (int64_t)k * 60000 + (int64_t)(mix(h ^ 4) % 30000);
    r->end_ms = r->start_ms + 1000 + (int64_t)(mix(h ^ 5) % 9000);
}

static void rec_free(rec_t *r) { free(r->cmd); free(r->meta); free(r->payload); }

/* Canonical line exactly as event_log formats it (fields are already clean). */
static char *rec_line(int64_t id, const char *chan, const char *dev, const char *tag, const char *cmd,
                      int64_t s, int64_t e, const char *meta, const char *payload, size_t *out_len)
{
    size_t cap = strlen(cmd) + strlen(meta) + strlen(payload) + 256;
    char *b = malloc(cap);
    int n = snprintf(b, cap, "%lld\t%s\t%s\t%s\t%s\t%lld\t%lld\t%s\t%s\n", (long long)id, chan, dev, tag,
                     cmd, (long long)s, (long long)e, meta, payload);
    *out_len = (size_t)n;
    return b;
}

static void hash_fields(FILE *out, const char *kind, int64_t id, const char *chan, const char *dev, const char *tag,
                        const char *cmd, int64_t s, int64_t e, const char *meta, const char *payload, const char *extra)
{
    char hc[65], hm[65], hp[65], hl[65];
    sha256_hex(cmd, strlen(cmd), hc);
    sha256_hex(meta, strlen(meta), hm);
    sha256_hex(payload, strlen(payload), hp);
    size_t ll;
    char *line = rec_line(id, chan, dev, tag, cmd, s, e, meta, payload, &ll);
    sha256_hex(line, ll, hl);
    free(line);
    fprintf(out, "{\"kind\":\"%s\",\"id\":%lld,\"channel\":\"%s\",\"device\":\"%s\",\"tag\":\"%s\",\"cmd_sha\":\"%s\","
                 "\"start_ms\":%lld,\"end_ms\":%lld,\"meta_sha\":\"%s\",\"payload_sha\":\"%s\",\"line_sha\":\"%s\"%s}\n",
            kind, (long long)id, chan, dev, tag, hc, (long long)s, (long long)e, hm, hp, hl, extra ? extra : "");
    fflush(out);
}

/* ── keeper ──────────────────────────────────────────────────────────────── */
static void keeper_pass(void)
{
#ifdef EVQ_BASELINE
    /* Baseline app_main keeper: legacy import, then the synced-only archive. */
    if (evq_sd_stub_mounted()) {
        (void)event_log_import_sd_backlog(4);
        if (event_log_archive_pending()) { size_t a = 0; (void)event_log_archive_to_sd(&a); }
    }
#else
    (void)event_log_sd_service();
#endif
}

static void keeper_auto_tick(void)
{
    if (!s_keeper_auto) return;
#ifdef EVQ_BASELINE
    /* The baseline keeper runs every 60 s; model ≈ one pass per 50 stores. */
    if (++s_stores_since_keeper >= 50) { s_stores_since_keeper = 0; keeper_pass(); }
#else
    if (event_log_sd_service_pending()) keeper_pass();
#endif
}

/* ── health ──────────────────────────────────────────────────────────────── */
static void health(const char *label)
{
    evlog_health_t h;
    esp_err_t err = event_log_health(&h);
    uint32_t rs = 0, ro = 0, ts = 0;
    event_log_cursor_info(&rs, &ro, &ts);
    uint64_t freeb = 0;
    event_log_free_bytes(&freeb);
    if (err != ESP_OK) { fprintf(s_hl, "{\"label\":\"%s\",\"err\":%d}\n", label, err); fflush(s_hl); return; }
#ifdef EVQ_BASELINE
    fprintf(s_hl, "{\"label\":\"%s\",\"baseline\":1,\"available\":%d,\"write_full\":%d,\"pending\":%lld,\"next_id\":%lld,"
                  "\"last_acked_id\":%lld,\"skipped\":%lld,\"dropped\":%lld,\"rd_seq\":%u,\"rd_off\":%u,\"tail_seq\":%u,"
                  "\"flash_free\":%llu,\"sd_ops\":%llu}\n",
            label, h.available, h.write_full, (long long)h.pending, (long long)h.next_id, (long long)h.last_acked_id,
            (long long)h.skipped, (long long)h.dropped, h.rd_seq, ro, h.tail_seq, (unsigned long long)freeb,
            (unsigned long long)shim_sd_ops());
#else
    char text[1024];
    int tl = evq_render_health_text(&h, text, sizeof text);
    fprintf(s_hl, "{\"label\":\"%s\",\"available\":%d,\"write_full\":%d,\"pending\":%lld,\"pending_exact\":%d,"
                  "\"deliverable_pending\":%lld,\"flash_pending\":%lld,\"sd_pending\":%lld,\"reimport_pending\":%lld,"
                  "\"next_id\":%lld,\"last_acked_id\":%lld,\"skipped\":%lld,\"dropped\":%lld,"
                  "\"refused_full\":%lld,\"refused_media\":%lld,\"refused_too_large\":%lld,\"refused_unavailable\":%lld,"
                  "\"quarantined_poison\":%lld,\"quarantined_malformed\":%lld,\"skipped_unindexed_gap\":%lld,"
                  "\"corrupt_detected\":%lld,\"corrupt_medium\":\"%s\",\"rd_seq\":%u,\"rd_off\":%u,\"tail_seq\":%u,"
                  "\"sd_state\":\"%s\",\"head_block\":\"%s\",\"storage_blocked\":%d,\"blocked_reason\":\"%s\","
                  "\"spool_files\":%u,\"spool_errors\":%u,\"mirror_used\":%u,\"reclaimed\":%u,\"archived\":%u,"
                  "\"reimported\":%u,\"pressure_notifies\":%u,\"sd_bursts\":%u,\"index_segments\":%u,\"index_cap\":%u,"
                  "\"flash_free\":%llu,\"sd_ops\":%llu,\"clock\":%u,\"render_len\":%d,\"boot\":%d,"
                  "\"sd_retired_names\":%u,\"sd_bad_copies\":%u}\n",
            label, h.available, h.write_full, (long long)h.pending, h.pending_exact, (long long)h.deliverable_pending,
            (long long)h.flash_pending, (long long)h.sd_pending, (long long)h.reimport_pending,
            (long long)h.next_id, (long long)h.last_acked_id, (long long)h.skipped, (long long)h.dropped,
            (long long)h.refused_full, (long long)h.refused_media, (long long)h.refused_too_large,
            (long long)h.refused_unavailable, (long long)h.quarantined_poison, (long long)h.quarantined_malformed,
            (long long)h.skipped_unindexed_gap, (long long)h.corrupt_detected, event_log_medium_name(h.corrupt_medium),
            h.rd_seq, ro, h.tail_seq, event_log_sd_state_name(h.sd_state), event_log_block_name(h.head_block),
            h.storage_blocked, event_log_blocked_reason_name(h.blocked_reason), h.spool_files, h.spool_errors,
            h.mirror_used, h.reclaimed_files, h.archived_files, h.reimported_files, h.pressure_notifies,
            h.sd_bursts, h.index_segments, h.index_cap, (unsigned long long)freeb,
            (unsigned long long)shim_sd_ops(), evq_clock_now(), tl, s_boot, h.sd_retired_names, h.sd_bad_copies);
#endif
    fflush(s_hl);
}

/* ── store ───────────────────────────────────────────────────────────────── */
static void do_store(unsigned n, const char *profile)
{
    for (unsigned i = 0; i < n; i++) {
        uint64_t k = s_gen++;
        rec_t r;
        gen_record(k, profile, &r);
        int64_t id = 0;
        if (event_log_next_id(&id) != ESP_OK) { fprintf(stderr, "next_id failed\n"); exit(4); }
        uint64_t call = ++s_calls;
        save_gen();
        {
            char extra[96];
            snprintf(extra, sizeof extra, ",\"k\":%" PRIu64 ",\"call\":%" PRIu64, k, call);
            hash_fields(s_att, "attempt", id, r.channel, r.device, r.tag, r.cmd, r.start_ms, r.end_ms, r.meta, r.payload, extra);
        }
        measurement_event_desc_t d = {
            .measure_id = id, .channel = r.channel, .device = r.device, .tag = r.tag, .cmd_raw = r.cmd,
            .start_ms = r.start_ms, .end_ms = r.end_ms, .metadata_json = r.meta, .payload_json = r.payload,
        };
        esp_err_t err = event_log_store_event(&d);
        if (err == ESP_OK) {
            char extra[160];
            uint32_t rs = 0, ro = 0, ts = 0;
            event_log_cursor_info(&rs, &ro, &ts);
            char tp[64];
            snprintf(tp, sizeof tp, "evstore/events/ev-%06u.log", (unsigned)ts);
            struct stat st;
            long long tsz = stat(tp, &st) == 0 ? (long long)st.st_size : -1;
            snprintf(extra, sizeof extra, ",\"k\":%" PRIu64 ",\"call\":%" PRIu64 ",\"tail\":\"%s\",\"tail_size\":%lld", k, call, tp, tsz);
            hash_fields(s_acc, "accepted", id, r.channel, r.device, r.tag, r.cmd, r.start_ms, r.end_ms, r.meta, r.payload, extra);
            char mk[96];
            snprintf(mk, sizeof mk, "store_ok %lld %s %lld", (long long)id, tp, tsz);
            shim_mark(mk);
        } else {
            fprintf(s_ref, "{\"k\":%" PRIu64 ",\"id\":%lld,\"call\":%" PRIu64 ",\"err\":%d,\"err_name\":\"%s\",\"boot\":%d}\n",
                    k, (long long)id, call, err, esp_err_to_name(err), s_boot);
            fflush(s_ref);
        }
        rec_free(&r);
        keeper_auto_tick();
    }
}

/* ── broker simulator ────────────────────────────────────────────────────── */
typedef struct { int64_t id; bool live; char *line; } slot_t;
static uint64_t s_brng = 1;
static uint64_t brand(void) { s_brng = mix(s_brng); return s_brng; }
static uint64_t s_deliv_n = 0;
static int64_t  s_hold[16];
static unsigned s_hold_n = 0;

/* deliver up to max records (0 = all reachable). Returns last claim code. */
static esp_err_t do_deliver(uint64_t max, double refuse, bool shuffle, unsigned disc_every, unsigned window)
{
    slot_t win[16];
    unsigned nw = 0, rounds = 0;
    uint64_t delivered = 0;
    esp_err_t last = ESP_OK;
    if (window == 0 || window > 16) window = 16;
    shim_mark("deliver_begin");
    for (unsigned guard = 0; guard < 1000000; guard++) {
        bool stop_claim = false;
        while (nw < window && !stop_claim && (max == 0 || delivered + nw < max)) {
            measurement_event_t e;
            esp_err_t rc = event_log_claim_next_event(&e);
            last = rc;
            if (rc == ESP_OK) {
                char extra[96];
                snprintf(extra, sizeof extra, ",\"n\":%" PRIu64, s_deliv_n);
                /* hash at claim; logged as delivered only on a clean PUBACK below */
                win[nw].id = e.measure_id;
                win[nw].live = true;
                char *tmpbuf = NULL; size_t tl = 0;
                FILE *mem = open_memstream(&tmpbuf, &tl);
                hash_fields(mem, "delivered", e.measure_id, e.channel, e.device, e.tag, e.cmd_raw ? e.cmd_raw : "",
                            e.start_ticks_ms, e.end_ticks_ms, e.metadata_json ? e.metadata_json : "",
                            e.payload_json ? e.payload_json : "", extra);
                fclose(mem);
                win[nw].line = tmpbuf;      /* logged as delivered only on a clean PUBACK */
                nw++;
                measurement_event_free(&e);
            } else {
                if (rc != ESP_ERR_INVALID_STATE) {
                    fprintf(s_clm, "{\"code\":%d,\"name\":\"%s\",\"outstanding\":%u}\n", rc, esp_err_to_name(rc), nw);
                    fflush(s_clm);
                }
                stop_claim = true;
            }
        }
        if (nw == 0) break;
        rounds++;
        /* Broker completion order */
        unsigned order[16];
        for (unsigned i = 0; i < nw; i++) order[i] = i;
        if (shuffle) for (unsigned i = nw; i > 1; i--) { unsigned j = (unsigned)(brand() % i); unsigned t = order[i-1]; order[i-1] = order[j]; order[j] = t; }
        bool disconnect = disc_every > 0 && (rounds % disc_every) == 0;
        for (unsigned oi = 0; oi < nw; oi++) {
            unsigned i = order[oi];
            if (disconnect) {
                event_log_mark_event_pending(win[i].id);
                continue;
            }
            double u = (double)(brand() % 1000000) / 1000000.0;
            if (u < refuse) {
                /* PUBACK reason 0x87: refused — stays pending */
                event_log_mark_event_pending(win[i].id);
                continue;
            }
            /* Clean PUBACK: the broker has it. Log before the local mark. */
            fputs(win[i].line, s_del);
            fflush(s_del);
            s_deliv_n++;
            char mk[64]; snprintf(mk, sizeof mk, "puback %lld", (long long)win[i].id); shim_mark(mk);
            esp_err_t m = event_log_mark_event_synced(win[i].id);
            if (m != ESP_OK) { fprintf(stderr, "mark_synced(%lld) -> %d\n", (long long)win[i].id, m); exit(5); }
            delivered++;
        }
        for (unsigned i = 0; i < nw; i++) { free(win[i].line); win[i].line = NULL; }
        nw = 0;
        keeper_auto_tick();
        if (max != 0 && delivered >= max) break;
        if (last != ESP_OK && last != ESP_ERR_INVALID_STATE && refuse <= 0 && !disconnect) {
            /* claim stopped (queue end / waiting) and everything outstanding completed */
            break;
        }
        if (rounds > 200000) break;
    }
    shim_mark("deliver_end");
    return last;
}

/* ── misc commands ───────────────────────────────────────────────────────── */
static void checkpoint(const char *label)
{
    char hl[80];
    snprintf(hl, sizeof hl, "cp:%s", label);
    health(hl);
    evq_host_flush_manifests();
    printf("CHECKPOINT %s\n", label);
    fflush(stdout);
    char buf[64];
    if (fgets(buf, sizeof buf, stdin) == NULL || strncmp(buf, "OK", 2) != 0) {
        fprintf(stderr, "checkpoint %s rejected by oracle: %s\n", label, buf);
        exit(7);
    }
}

static void sd_cmd(char *arg1, char *arg2, char *arg3)
{
    uint32_t cid = evq_sd_stub_cid();
    if (strcmp(arg1, "remove") == 0) evq_sd_stub_set(false, cid);
    else if (strcmp(arg1, "insert") == 0) { mkdir("sdcard", 0777); evq_sd_stub_set(true, cid); }
#ifndef EVQ_BASELINE
    else if (strcmp(arg1, "park") == 0) { event_log_set_sd_parked(true); evq_sd_stub_set(false, cid); }
    else if (strcmp(arg1, "unpark") == 0) { evq_sd_stub_set(true, cid); event_log_set_sd_parked(false); }
#endif
    else if (strcmp(arg1, "lost") == 0) evq_sd_stub_set_lost(arg2 && atoi(arg2) != 0);
    else if (strcmp(arg1, "swap") == 0 && arg2 && arg3) {
        /* current card → cards/<cid>, cards/<arg2> → sdcard */
        mkdir("cards", 0777);
        char old[64], neu[64];
        snprintf(old, sizeof old, "cards/%08x", (unsigned)cid);
        snprintf(neu, sizeof neu, "cards/%s", arg2);
        rename("sdcard", old);
        if (rename(neu, "sdcard") != 0) mkdir("sdcard", 0777);
        evq_sd_stub_set(true, (uint32_t)strtoul(arg3, NULL, 16));
        shim_rescan();
    } else if (strcmp(arg1, "fill") == 0 && arg2) {
        FILE *f = fopen("sdcard/filler.bin", "w");
        if (f) { if (ftruncate(fileno(f), atoll(arg2)) != 0) perror("fill"); fclose(f); }
        shim_rescan();
    } else if (strcmp(arg1, "unfill") == 0) {
        remove("sdcard/filler.bin");
        shim_rescan();
    } else {
        fprintf(stderr, "bad sd command %s\n", arg1);
        exit(6);
    }
#ifndef EVQ_BASELINE
    event_log_sd_notify();
#endif
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: evq_driver <script> <start_line>\n"); return 2; }
    const char *sc = getenv("EVQ_SCENARIO");
    if (sc) snprintf(s_scenario, sizeof s_scenario, "%s", sc);
    const char *sd = getenv("EVQ_SEED");
    if (sd) s_seed = strtoull(sd, NULL, 10);
    const char *bs = getenv("EVQ_BOOT");
    s_boot = bs ? atoi(bs) : 0;
    shim_seed(s_seed * 7919 + (bs ? strtoull(bs, NULL, 10) : 0));
    s_brng = mix(s_seed ^ 0xB10C) + (bs ? strtoull(bs, NULL, 10) : 0);
    mkdir("out", 0777);
    mkdir("evstore", 0777);
    s_acc = open_out("accepted.jsonl"); s_ref = open_out("refused.jsonl"); s_att = open_out("attempts.jsonl");
    s_del = open_out("delivered.jsonl"); s_hl = open_out("health.jsonl"); s_clm = open_out("claims.jsonl");
    load_gen();

    if (event_log_init() != ESP_OK) { fprintf(stderr, "event_log_init failed\n"); return 3; }
    shim_mark("boot");

    FILE *sf = fopen(argv[1], "r");
    if (sf == NULL) { perror(argv[1]); return 2; }
    long start = atol(argv[2]);
    char line[512];
    long ln = 0;
    while (fgets(line, sizeof line, sf) != NULL) {
        ln++;
        if (ln < start) continue;
        char *nl = strchr(line, '\n'); if (nl) *nl = '\0';
        FILE *pc = fopen("out/pc", "w"); if (pc) { fprintf(pc, "%ld\n", ln); fclose(pc); }
        char *cmd = strtok(line, " ");
        if (cmd == NULL || cmd[0] == '#') continue;
        char *a1 = strtok(NULL, " "), *a2 = strtok(NULL, " "), *a3 = strtok(NULL, " ");
        char *a4 = strtok(NULL, " "), *a5 = strtok(NULL, " ");
        char mk[160]; snprintf(mk, sizeof mk, "cmd %ld %s", ln, cmd); shim_mark(mk);
        if (strcmp(cmd, "store") == 0) do_store((unsigned)atoi(a1), a2 ? a2 : "default");
        else if (strcmp(cmd, "keeper") == 0) s_keeper_auto = a1 && strcmp(a1, "auto") == 0;
        else if (strcmp(cmd, "service") == 0) { int n = a1 ? atoi(a1) : 1; for (int i = 0; i < n; i++) keeper_pass(); }
        else if (strcmp(cmd, "deliver") == 0) {
            uint64_t max = (a1 && strcmp(a1, "all") != 0) ? strtoull(a1, NULL, 10) : 0;
            double refuse = 0; bool shuffle = false; unsigned disc = 0, window = 16;
            char *opts[4] = { a2, a3, a4, a5 };
            for (int i = 0; i < 4; i++) {
                if (!opts[i]) continue;
                if (strncmp(opts[i], "refuse=", 7) == 0) refuse = atof(opts[i] + 7);
                else if (strncmp(opts[i], "shuffle=", 8) == 0) shuffle = atoi(opts[i] + 8) != 0;
                else if (strncmp(opts[i], "disc=", 5) == 0) disc = (unsigned)atoi(opts[i] + 5);
                else if (strncmp(opts[i], "window=", 7) == 0) window = (unsigned)atoi(opts[i] + 7);
            }
            do_deliver(max, refuse, shuffle, disc, window);
        }
        else if (strcmp(cmd, "drain_all") == 0) {
            /* Recovery epilogue: keeper + delivery until nothing is owed. */
            int limit = a1 ? atoi(a1) : 400;
            for (int i = 0; i < limit; i++) {
                keeper_pass();
                do_deliver(0, 0, false, 0, 16);
                evq_clock_advance(61000);
                evlog_health_t h;
                if (event_log_health(&h) == ESP_OK) {
#ifdef EVQ_BASELINE
                    if (h.pending == 0 && i > 2) {
                        bool legacy_left = false;
                        struct stat st;
                        if (stat("sdcard/events", &st) == 0) {
                            FILE *p = popen("ls sdcard/events 2>/dev/null | grep -c '^ev-.*\\.log$'", "r");
                            if (p) { int c = 0; if (fscanf(p, "%d", &c) == 1) legacy_left = c > 0; pclose(p); }
                        }
                        if (!legacy_left) break;
                    }
#else
                    if (h.pending == 0 && h.reimport_pending == 0 && h.pending_exact && i > 1) break;
#endif
                }
            }
        }
        else if (strcmp(cmd, "sd") == 0) sd_cmd(a1, a2, a3);
        else if (strcmp(cmd, "tick") == 0) evq_clock_advance((uint32_t)atoi(a1));
        else if (strcmp(cmd, "health") == 0) health(a1 ? a1 : "h");
        else if (strcmp(cmd, "checkpoint") == 0) checkpoint(a1 ? a1 : "cp");
        else if (strcmp(cmd, "claimcode") == 0) {
            measurement_event_t e;
            esp_err_t rc = event_log_claim_next_event(&e);
            fprintf(s_clm, "{\"label\":\"%s\",\"code\":%d,\"name\":\"%s\"}\n", a1 ? a1 : "", rc, esp_err_to_name(rc));
            fflush(s_clm);
            if (rc == ESP_OK) { event_log_mark_event_pending(e.measure_id); measurement_event_free(&e); }
        }
        else if (strcmp(cmd, "claimhold") == 0) {
            /* E6/E9: claim N (published, no PUBACK yet) and keep them outstanding */
            unsigned n = a1 ? (unsigned)atoi(a1) : 1;
            for (unsigned i = 0; i < n && s_hold_n < 16; i++) {
                measurement_event_t e;
                esp_err_t rc = event_log_claim_next_event(&e);
                if (rc != ESP_OK) break;
                s_hold[s_hold_n++] = e.measure_id;
                fprintf(s_clm, "{\"label\":\"claimhold\",\"id\":%lld}\n", (long long)e.measure_id);
                fflush(s_clm);
                measurement_event_free(&e);
            }
        }
        else if (strcmp(cmd, "revert") == 0) {
            for (unsigned i = 0; i < s_hold_n; i++) event_log_mark_event_pending(s_hold[i]);
            s_hold_n = 0;
        }
        else if (strcmp(cmd, "store_poison") == 0) {
            /* Q1: a record outside the accepted set whose parse allocation fails */
            int64_t id = 0;
            event_log_next_id(&id);
            char payload[160];
            snprintf(payload, sizeof payload, "{\"synthetic_test\":true,\"oom_poison\":true,\"k\":%lld}", (long long)id);
            measurement_event_desc_t d = { .measure_id = id, .channel = "uart_0", .device = "28:37:2F:FF:E7:04",
                                           .tag = "MEASUREMENT", .cmd_raw = "", .start_ms = 1758000000000LL,
                                           .end_ms = 1758000001000LL, .metadata_json = "", .payload_json = payload };
            esp_err_t rc = event_log_store_event(&d);
            FILE *pf = open_out("poison.jsonl");
            fprintf(pf, "{\"id\":%lld,\"rc\":%d}\n", (long long)id, rc);
            fclose(pf);
        }
        else if (strcmp(cmd, "ackstale") == 0) {
            /* E5: acks for ids not in the window must be refused with no effect */
            int64_t id = a1 ? atoll(a1) : 999999999;
            esp_err_t r1 = event_log_mark_event_synced(id), r2 = event_log_mark_event_pending(id);
            fprintf(s_clm, "{\"label\":\"ackstale\",\"synced\":%d,\"pending\":%d}\n", r1, r2);
            fflush(s_clm);
        }
        else if (strcmp(cmd, "rewind") == 0) {
            uint32_t ns = 0; int64_t p = 0;
            esp_err_t r = event_log_rewind((uint32_t)atoi(a1 ? a1 : "0"), &ns, &p);
            fprintf(s_clm, "{\"label\":\"rewind\",\"code\":%d,\"seq\":%u,\"pending\":%lld}\n", r, ns, (long long)p);
            fflush(s_clm);
        }
        else if (strcmp(cmd, "reboot") == 0) {
            event_log_prepare_shutdown();
            evq_host_flush_manifests();
            FILE *p2 = fopen("out/pc", "w"); if (p2) { fprintf(p2, "%ld\n", ln); fclose(p2); }
            return 85;
        }
        else if (strcmp(cmd, "crash") == 0) { evq_host_flush_manifests(); shim_mark("crash-cmd"); _exit(86); }
        else { fprintf(stderr, "unknown command %s\n", cmd); return 2; }
    }
    fclose(sf);
    /* Script end = the device is shut down gracefully (esp_restart runs the
     * shutdown handler); only EXIT 86 models an ungraceful power loss. */
    event_log_prepare_shutdown();
    evq_host_flush_manifests();
    if (evq_sd_stub_refs() != 0) { fprintf(stderr, "FATAL: SD refs leaked: %d\n", evq_sd_stub_refs()); return 9; }
    return 0;
}
