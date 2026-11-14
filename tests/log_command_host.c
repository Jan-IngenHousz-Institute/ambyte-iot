/* Production log_list / log_read (components/command_router/log_command.c)
 * against a real directory, with the ESP-IDF cJSON. Only the resident-worker
 * queue, the maintenance lock and the SD probes are replaced; every reply is
 * printed as "REPLY <json>" for tests/test_log_command.py. */
#include "log_command.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void (*g_job)(void *);
static void *g_arg;
static int g_queued, g_maint_held, g_io_refs, g_io_begins;
static bool g_deny_maint, g_parked, g_mounted = true, g_lost, g_io_refuse, g_queue_full;

static void reply(const char *json) { printf("REPLY %s\n", json); }
static bool submit(void (*run)(void *), void *arg)
{
    if (g_queue_full) return false;
    assert(g_job == NULL);
    g_job = run; g_arg = arg; g_queued++;
    return true;
}
static bool maint_begin(void) { if (g_deny_maint) return false; g_maint_held++; return true; }
static void maint_end(void) { g_maint_held--; }
static bool mounted(void) { return g_mounted; }
static bool parked(void) { return g_parked; }
static bool lost(void) { return g_lost; }
static bool io_begin(void) { if (g_io_refuse) return false; g_io_refs++; g_io_begins++; return true; }
static void io_end(void) { g_io_refs--; }

static void request(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    assert(root);
    assert(log_command_handle(root));
    cJSON_Delete(root);
}

/* The maintenance worker runs the queued job; locks must balance. */
static void run_job(void)
{
    assert(g_job);
    void (*run)(void *) = g_job;
    g_job = NULL;
    run(g_arg);
    assert(g_maint_held == 0 && g_io_refs == 0);
}

static void req_run(const char *json) { request(json); run_job(); }

static void pure(void)
{
    const char *good[] = {"ambyte.log", "ambyte.1.log", "a", "A-b_c.9", NULL};
    const char *bad[] = {"", ".", "..", "../x", "a/b", "/etc", "a\\b", ".hidden", "a..b", "x:y",
                         "sp ace", "quo\"te", "\xc3\xa9.log", NULL};
    for (int i = 0; good[i]; i++) assert(log_command_name_valid(good[i]));
    for (int i = 0; bad[i]; i++) assert(!log_command_name_valid(bad[i]));
    char longname[80];
    memset(longname, 'a', 64); longname[64] = '\0';
    assert(log_command_name_valid(longname));
    longname[64] = 'a'; longname[65] = '\0';
    assert(!log_command_name_valid(longname));
    assert(!log_command_name_valid(NULL));

    int64_t s, l;
    assert(!log_command_resolve(100, 0, 4096, &s, &l) && s == 0 && l == 100);
    assert(!log_command_resolve(100, -10, 4096, &s, &l) && s == 90 && l == 10);
    assert(!log_command_resolve(100, -500, 4096, &s, &l) && s == 0 && l == 100);    /* tail > file */
    assert(!log_command_resolve(100, 100, 4096, &s, &l) && s == 100 && l == 0);     /* at EOF */
    assert(!strcmp(log_command_resolve(100, 101, 4096, &s, &l), "offset_past_eof"));
    assert(!log_command_resolve(100000, 10, 100000, &s, &l) && s == 10 && l == 8192); /* cap */
    assert(!strcmp(log_command_resolve(100, 0, 0, &s, &l), "invalid_length"));
    assert(!strcmp(log_command_resolve(100, 0, -1, &s, &l), "invalid_length"));

    /* RFC 4648 §10 vectors */
    const char *in[] = {"", "f", "fo", "foo", "foob", "fooba", "foobar"};
    const char *exp[] = {"", "Zg==", "Zm8=", "Zm9v", "Zm9vYg==", "Zm9vYmE=", "Zm9vYmFy"};
    for (int i = 0; i < 7; i++) {
        char out[16];
        size_t n = log_command_base64((const uint8_t *)in[i], strlen(in[i]), out, sizeof out);
        assert(n == strlen(exp[i]) && !strcmp(out, exp[i]));
    }
    char tiny[4];
    assert(log_command_base64((const uint8_t *)"foo", 3, tiny, sizeof tiny) == 0);  /* needs 5 */
    puts("PURE ok");
}

int main(int argc, char **argv)
{
    assert(argc == 3);
    log_command_config_t cfg = {
        .reply = reply, .submit = submit, .maintenance_begin = maint_begin, .maintenance_end = maint_end,
        .dir = argv[1], .sd_mounted = mounted, .sd_parked = parked, .sd_io_lost = lost,
        .io_begin = io_begin, .io_end = io_end,
    };
    log_command_init(&cfg);
    const char *s = argv[2];
    if (!strcmp(s, "pure")) {
        pure();
    } else if (!strcmp(s, "list")) {
        req_run("{\"type\":\"log_list\",\"id\":\"l-\\\"1\"}");
    } else if (!strcmp(s, "reads")) {
        req_run("{\"type\":\"log_read\",\"id\":\"r1\",\"name\":\"ambyte.log\"}");                       /* defaults */
        req_run("{\"type\":\"log_read\",\"id\":\"r2\",\"name\":\"ambyte.log\",\"offset\":5000,\"length\":100}");
        req_run("{\"type\":\"log_read\",\"id\":\"r3\",\"name\":\"ambyte.log\",\"offset\":-300}");       /* tail */
        req_run("{\"type\":\"log_read\",\"id\":\"r4\",\"name\":\"ambyte.log\",\"offset\":0,\"length\":100000}"); /* cap */
        req_run("{\"type\":\"log_read\",\"id\":\"r5\",\"name\":\"ambyte.log\",\"offset\":20000}");      /* exact EOF */
        req_run("{\"type\":\"log_read\",\"id\":\"r6\",\"name\":\"ambyte.log\",\"offset\":20001}");      /* past EOF */
        req_run("{\"type\":\"log_read\",\"id\":\"r7\",\"name\":\"ambyte.1.log\",\"offset\":-99999,\"length\":8192}");
        req_run("{\"type\":\"log_read\",\"id\":\"r8\",\"name\":\"binary.log\",\"length\":256}");
        req_run("{\"type\":\"log_read\",\"id\":\"r9\",\"name\":\"empty.log\"}");
        req_run("{\"type\":\"log_read\",\"id\":\"r10\",\"name\":\"missing.log\"}");                   /* not in dir */
        req_run("{\"type\":\"log_read\",\"id\":\"r11\",\"name\":\"subdir\"}");                         /* a directory */
    } else if (!strcmp(s, "invalid")) {
        /* Refused on the MQTT task: nothing is ever queued, no SD touched. */
        const char *bad[] = {
            "{\"type\":\"log_read\",\"id\":\"b1\",\"name\":\"../secret.txt\"}",
            "{\"type\":\"log_read\",\"id\":\"b2\",\"name\":\"/sdcard/events/ev-000001.log\"}",
            "{\"type\":\"log_read\",\"id\":\"b3\",\"name\":\"..\"}",
            "{\"type\":\"log_read\",\"id\":\"b4\",\"name\":\"evq/m-1.log\"}",
            "{\"type\":\"log_read\",\"id\":\"b5\",\"name\":42}",
            "{\"type\":\"log_read\",\"id\":\"b6\"}",
            "{\"type\":\"log_read\",\"id\":\"b7\",\"name\":\"ambyte.log\",\"offset\":1.5}",
            "{\"type\":\"log_read\",\"id\":\"b8\",\"name\":\"ambyte.log\",\"offset\":\"0\"}",
            "{\"type\":\"log_read\",\"id\":\"b9\",\"name\":\"ambyte.log\",\"length\":0}",
            "{\"type\":\"log_read\",\"id\":\"b10\",\"name\":\"ambyte.log\",\"length\":-5}",
            "{\"type\":\"log_read\",\"id\":\"b11\",\"name\":\"ambyte.log\",\"offset\":1e12}",
            "{\"type\":\"log_read\",\"name\":\"ambyte.log\"}",
            "{\"type\":\"log_list\",\"id\":\"\"}",
            NULL,
        };
        for (int i = 0; bad[i]; i++) request(bad[i]);
        assert(g_queued == 0 && g_io_begins == 0);
        cJSON *other = cJSON_Parse("{\"type\":\"ping\",\"id\":\"p\"}");
        assert(!log_command_handle(other));                     /* not ours */
        cJSON_Delete(other);
    } else if (!strcmp(s, "refusals")) {
        const char *rd = "{\"type\":\"log_read\",\"id\":\"x\",\"name\":\"ambyte.log\"}";
        g_parked = true; g_mounted = false; req_run(rd); g_parked = false;       /* parked (and unmounted) */
        req_run("{\"type\":\"log_list\",\"id\":\"x\"}");                           /* absent */
        g_mounted = true;
        g_lost = true; req_run(rd); g_lost = false;
        g_io_refuse = true; req_run(rd); g_io_refuse = false;
        g_deny_maint = true; req_run(rd); g_deny_maint = false;
        assert(g_io_begins == 0);                               /* never touched the card */
        request(rd); request(rd);                               /* second while first queued */
        run_job();
        g_queue_full = true; request(rd); g_queue_full = false; /* worker queue full */
        req_run(rd);                                            /* busy flag released again */
    } else {
        return 2;
    }
    return 0;
}
