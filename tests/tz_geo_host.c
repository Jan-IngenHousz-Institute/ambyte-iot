/* Host harness for the production tz_geo reader (components/timezone/tz_geo.c).
 * Usage: tz_geo_host <blob> [truncate_to]   then "lat lon" lines on stdin;
 * prints one zone name (or "-") per line. The blob is copied into an exact-size
 * heap buffer so ASan flags any read past its end. */
#include "tz_geo.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 2;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) return 2;
    size_t len = (size_t)size;
    if (argc > 2) {
        size_t cut = strtoul(argv[2], NULL, 10);
        if (cut < len) len = cut;
    }
    uint8_t *blob = malloc(len ? len : 1);
    if (!blob || fread(blob, 1, len, f) != len) return 2;
    fclose(f);
    double lat, lon;
    while (scanf("%lf %lf", &lat, &lon) == 2) {
        const char *zone = tz_geo_lookup(blob, len, lat, lon);
        puts(zone ? zone : "-");
    }
    free(blob);
    return 0;
}
