/*
 * tz_geo.c — reader for the generated coordinate -> zone quadtree (format 1).
 * The format, its source and the accuracy bound are documented once, in
 * tools/gen_tz_geo.py; flash_gui/tz_geo.py is the same algorithm in Python and
 * tests/test_tz_geo_c.py holds the two to identical answers.
 *
 * Every read is bounds-checked against the blob and the tile: the map is
 * compiled in, but a lookup must never walk off it whatever the bytes say.
 * Runs at boot and on a location change only (a few thousand bit reads).
 */

#include "tz_geo.h"

#include <math.h>
#include <string.h>

#define TZ_GEO_HEADER   64U
#define TZ_GEO_NONE     0xFFFFU
#define TZ_GEO_MAX_DEPTH      24U   /* finest level the reader accepts */
#define TZ_GEO_MAX_TILE_DEPTH 12U   /* 4^12 tiles: far beyond any embedded map */

static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint16_t rd_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

bool tz_geo_location_usable(double lat, double lon)
{
    if (!isfinite(lat) || !isfinite(lon)) return false;
    if (lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) return false;
    /* (0, 0) is what a tool writes when it has nothing (and is open ocean in
     * the Gulf of Guinea anyway): never a deployment. */
    return !(lat == 0.0 && lon == 0.0);
}

/* Same IEEE-754 double expression as flash_gui/tz_geo.py::_cell_index, so the
 * firmware and the GUI land in the same finest cell. */
static uint32_t cell_index(double value, double origin, double span, unsigned depth)
{
    double n = (double)(1UL << depth);
    double idx = floor((value + origin) / span * n);
    if (idx < 0.0) return 0;
    if (idx > n - 1.0) return (uint32_t)(n - 1.0);
    return (uint32_t)idx;
}

typedef struct {
    const uint8_t *blob;
    uint64_t pos;    /* bit position */
    uint64_t limit;  /* bit limit (tile end) */
    unsigned width;
} bits_t;

/* Next symbol, or -1 past the tile. */
static int read_symbol(bits_t *b)
{
    if (b->pos + b->width > b->limit) return -1;
    int v = 0;
    for (unsigned i = 0; i < b->width; i++, b->pos++) {
        v = (v << 1) | ((b->blob[b->pos >> 3] >> (7 - (b->pos & 7))) & 1);
    }
    return v;
}

const char *tz_geo_lookup(const uint8_t *blob, size_t len, double lat, double lon)
{
    if (blob == NULL || len < TZ_GEO_HEADER || !tz_geo_location_usable(lat, lon)) return NULL;
    if (memcmp(blob, "TZG1", 4) != 0 || blob[4] != 1) return NULL;
    unsigned tile_depth = blob[5];
    unsigned max_depth = blob[6];
    uint16_t n_names = rd_u16(blob + 8);
    uint32_t names_off = rd_u32(blob + 52);
    uint32_t tiles_off = rd_u32(blob + 56);
    uint32_t size = rd_u32(blob + 60);
    /* Dimensions are checked before any shift or index, in 64-bit
     * arithmetic. A tile depth beyond TZ_GEO_MAX_TILE_DEPTH cannot be a real
     * map: 4^13 four-byte offsets alone exceed any blob we embed, and at 16
     * the tile count would no longer fit 32 bits (gpt-6-astra review: a
     * forged depth-16 header wrapped the count to 0 and read out of bounds). */
    if (size != len || tile_depth == 0 || tile_depth > TZ_GEO_MAX_TILE_DEPTH ||
        tile_depth > max_depth || max_depth > TZ_GEO_MAX_DEPTH) {
        return NULL;
    }
    const uint64_t n_tiles = 1ULL << (2 * tile_depth);
    const uint64_t dir_end = (uint64_t)tiles_off + 4ULL * n_tiles;
    if (names_off < TZ_GEO_HEADER || (uint64_t)names_off + 2ULL * n_names > tiles_off ||
        dir_end > size) {
        return NULL;
    }

    uint32_t x = cell_index(lon, 180.0, 360.0, max_depth);
    uint32_t y = cell_index(lat, 90.0, 180.0, max_depth);
    unsigned shift = max_depth - tile_depth;
    const uint64_t tile = ((uint64_t)(y >> shift) << tile_depth) + (x >> shift);
    if (tile >= n_tiles) return NULL;
    uint32_t start = rd_u32(blob + tiles_off + 4 * tile);
    uint32_t end = (tile + 1 == n_tiles) ? size : rd_u32(blob + tiles_off + 4 * tile + 4);
    if (start < dir_end || start >= end || end > size) return NULL;

    unsigned k = blob[start];
    if (k == 0 || (uint64_t)start + 1 + 2ULL * k > end) return NULL;
    const uint8_t *palette = blob + start + 1;
    unsigned width = 0;
    while ((1U << width) <= k) width++;   /* bit_length(k) */
    bits_t b = { .blob = blob, .pos = ((uint64_t)start + 1 + 2ULL * k) * 8,
                 .limit = (uint64_t)end * 8, .width = width };

    uint16_t zone = TZ_GEO_NONE;
    for (unsigned depth = tile_depth;; depth++) {
        int s = read_symbol(&b);
        if (s < 0 || (unsigned)s > k) return NULL;
        if (s != 0) {
            zone = rd_u16(palette + 2 * (s - 1));
            break;
        }
        if (depth >= max_depth) return NULL;   /* internal node below the finest level */
        unsigned level = max_depth - depth - 1;
        unsigned child = ((y >> level) & 1U) * 2U + ((x >> level) & 1U);
        for (unsigned c = 0; c < child; c++) {  /* skip the preceding siblings */
            uint32_t pending = 1;
            while (pending) {
                int t = read_symbol(&b);
                if (t < 0 || (unsigned)t > k) return NULL;
                if (t == 0) pending += 3; else pending--;
            }
        }
    }
    if (zone == TZ_GEO_NONE || zone >= n_names) return NULL;
    uint32_t name_at = names_off + rd_u16(blob + names_off + 2 * zone);
    if (name_at >= tiles_off) return NULL;
    const uint8_t *name = blob + name_at;
    const uint8_t *nul = memchr(name, '\0', tiles_off - name_at);
    if (nul == NULL || nul == name) return NULL;
    /* IANA names are [A-Za-z0-9/_+-]; anything else is a damaged table. */
    for (const uint8_t *c = name; c < nul; c++) {
        if (!((*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') ||
              (*c >= '0' && *c <= '9') || *c == '/' || *c == '_' || *c == '+' || *c == '-')) {
            return NULL;
        }
    }
    return (const char *)name;
}
