/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "svc_store.h"
#include "svc_wire.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static unsigned cuts;
void svc_store_fault(const char *point)
{
    (void)point;
    const char *cut = getenv("SVC_CUT");
    if (cut && ++cuts == strtoul(cut, NULL, 10)) _exit(86);
}

static size_t free_bytes(void *context)
{
    (void)context;
    const char *limit = getenv("SVC_FREE");
    return limit ? strtoul(limit, NULL, 10) : 512 * 1024;
}

int main(int argc, char **argv)
{
    assert(argc >= 3);
    svc_store_t store;
    uint64_t fresh = getenv("SVC_INSTANCE") ? strtoull(getenv("SVC_INSTANCE"), NULL, 16) : 1;
    const char *error = svc_store_open(&store, argv[1], fresh, free_bytes, NULL);
    svc_record_t r = {0};
    bool reused = false;
    if (!error && !strcmp(argv[2], "admit")) {
        assert(argc == 6);
        r.id = strtoull(argv[3], NULL, 16);
        r.kind = SVC_SCHEDULE; r.state = SVC_STAGING;
        r.size = (uint32_t)strtoul(argv[4], NULL, 10);
        memset(r.fingerprint, atoi(argv[5]), sizeof r.fingerprint);
        strcpy(r.version, "a3"); strcpy(r.built_fw, "test");
        r.saved = r.connected = r.applied = SVC_UNKNOWN;
        error = svc_store_admit(&store, &r, &reused);
    } else if (!error && argc > 3) {
        error = svc_store_get(&store, strtoull(argv[3], NULL, 16), &r);
        if (!error && !strcmp(argv[2], "put")) {
            assert(argc == 7);
            unsigned char data[145];
            size_t length = strlen(argv[5]) / 2;
            assert(length <= sizeof data);
            for (size_t i = 0; i < length; ++i) {
                unsigned n; assert(sscanf(argv[5] + 2 * i, "%2x", &n) == 1);
                data[i] = (unsigned char)n;
            }
            error = svc_store_put(&store, &r, (uint32_t)strtoul(argv[4], NULL, 10),
                                  (uint32_t)strtoul(argv[6], NULL, 16), data, length);
            svc_wipe(data, sizeof data);
        } else if (!error && !strcmp(argv[2], "abort")) error = svc_store_abort(&store, &r);
        else if (!error && !strcmp(argv[2], "finish")) {
            r.state = SVC_SUCCEEDED;
            strcpy(r.reason, "verified");
            error = svc_store_save(&store, &r);
        }
    }
    if (error) printf("{\"error\":\"%s\",\"highest\":%llu}\n", error, (unsigned long long)store.highest);
    else printf("{\"store\":%llu,\"highest\":%llu,\"id\":%llu,\"state\":\"%s\",\"offset\":%lu,\"reused\":%s}\n",
                (unsigned long long)store.instance, (unsigned long long)store.highest,
                (unsigned long long)r.id, svc_state_name(r.state), (unsigned long)r.offset,
                reused ? "true" : "false");
}
