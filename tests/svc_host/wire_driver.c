/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "svc_wire.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void dump(const unsigned char *bytes, size_t n)
{
    for (size_t i = 0; i < n; ++i) printf("%02x", bytes[i]);
}

int main(int argc, char **argv)
{
    assert(argc >= 3);
    size_t length = strlen(argv[2]) / 2;
    unsigned char *data = calloc(length + 1, 1);
    assert(data);
    for (size_t i = 0; i < length; ++i) {
        unsigned b;
        assert(sscanf(argv[2] + i * 2, "%2x", &b) == 1);
        data[i] = (unsigned char)b;
    }
    if (!strcmp(argv[1], "crc")) printf("%08x\n", svc_crc32(0, data, length));
    else if (!strcmp(argv[1], "utf8")) printf("%d\n", svc_utf8(data, length));
    else if (!strcmp(argv[1], "b64")) {
        unsigned char out[144] = {0};
        int n = svc_base64_decode((const char *)data, out, sizeof out);
        printf("%d ", n);
        if (n >= 0) dump(out, (size_t)n);
        puts("");
    } else if (!strcmp(argv[1], "filter") || !strcmp(argv[1], "trace")) {
        svc_editor_filter_t filter = {0};
        for (size_t i = 0; i < length; ++i) {
            unsigned char out[9] = {0};
            bool dropped;
            size_t n = svc_editor_byte(&filter, data[i], out, &dropped);
            dump(out, n);
            if (!strcmp(argv[1], "trace")) printf("%s\n", dropped ? ":dropped" : "");
        }
        if (!strcmp(argv[1], "filter")) puts("");
    } else {
        svc_rx_t rx = {0};
        for (size_t i = 0; i < length; ++i) {
            char *body;
            uint32_t correlation;
            const char *reason = svc_rx_byte(&rx, data[i], (int64_t)i, &body, &correlation);
            if (reason) printf("E %s\n", reason);
            if (body) {
                printf("F %08x %s\n", correlation, body);
                svc_rx_clear(&rx);
            }
        }
        if (argc > 3) {
            const char *reason = svc_rx_timeout(&rx, strtoll(argv[3], NULL, 10));
            if (reason) printf("E %s\n", reason);
        }
        svc_rx_clear(&rx);
        for (size_t i = 0; i < sizeof rx; ++i) assert(((unsigned char *)&rx)[i] == 0);
    }
    svc_wipe(data, length + 1);
    free(data);
}
