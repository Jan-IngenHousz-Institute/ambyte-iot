#include "ota_manifest.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "decode")) {
        size_t n = strlen(argv[2]) / 2;
        uint8_t bytes[256];
        if (n > sizeof bytes) return 2;
        for (size_t i = 0; i < n; ++i) {
            unsigned v;
            if (sscanf(argv[2] + 2 * i, "%2x", &v) != 1) return 2;
            bytes[i] = (uint8_t)v;
        }
        ota_manifest_t m;
        if (!ota_manifest_decode(bytes, n, &m)) puts("invalid");
        else printf("%u,%u,%u\n", m.readable, m.written, m.fallback);
        return 0;
    }
    if (argc == 9 && !strcmp(argv[1], "decide")) {
        ota_manifest_t r = {strtoul(argv[2],0,0),strtoul(argv[3],0,0),strtoul(argv[4],0,0)};
        ota_manifest_t c = {strtoul(argv[5],0,0),strtoul(argv[6],0,0),strtoul(argv[7],0,0)};
        const char *reason = ota_manifest_decide(&r, &c, strtoul(argv[8],0,0));
        puts(reason ? reason : "ok");
        return 0;
    }
    return 2;
}
