/*
 * tz_geo_embedded.c — binds tz_geo_zone_for() to the map compiled into app
 * flash rodata (~220 KB, tz_geo.bin via .incbin; see CMakeLists.txt for why
 * not EMBED_FILES). Kept apart from tz_geo.c so host tests link the reader
 * against the checked-in file instead.
 */

#include "tz_geo.h"

#ifndef TZ_GEO_BIN_PATH
#error "TZ_GEO_BIN_PATH must name components/timezone/tz_geo.bin (set by CMakeLists.txt)"
#endif

__asm__(".section .rodata.tz_geo_bin,\"a\"\n"
        ".balign 4\n"
        ".global tz_geo_bin_start\n"
        "tz_geo_bin_start:\n"
        ".incbin \"" TZ_GEO_BIN_PATH "\"\n"
        ".global tz_geo_bin_end\n"
        "tz_geo_bin_end:\n"
        ".previous\n");

extern const uint8_t tz_geo_bin_start[];
extern const uint8_t tz_geo_bin_end[];

const char *tz_geo_zone_for(double lat, double lon)
{
    return tz_geo_lookup(tz_geo_bin_start, (size_t)(tz_geo_bin_end - tz_geo_bin_start),
                         lat, lon);
}
