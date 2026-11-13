#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only
"""Generate the offline coordinate -> IANA timezone dataset (tz_geo).

WHY OFFLINE, WHY ON THE DEVICE
------------------------------
Ambytes are provisioned on a laptop in Europe and then shipped. The provisioning
image stamps the laptop's zone, so the Uluru deployment (2026-11) ran on
Europe/Berlin after operators had already sent the real coordinates with
`set_location`: the firmware stored the position but never translated it into a
zone, and day/night plus solar scheduling stayed 8.5 h out. The device has no
GPS; its coordinates arrive by MQTT `set_location`, the USB CLI or the
provisioning image. An online lookup API would add a third-party dependency and
would not work before the first connection or on a flaky field uplink, so the
firmware carries its own map. The flash GUI reads the very same bytes
(flash_gui/tz_geo.py) so the two can never disagree about a coordinate.

SOURCE
------
timezone-boundary-builder (https://github.com/evansiroky/timezone-boundary-builder)
release `TBB_RELEASE`, asset `timezones.geojson.zip` (land zones only, territorial
waters included, no ocean zones), pinned by SHA-256. Its boundaries come from
OpenStreetMap and are released under the ODbL; the release matches the IANA
tzdata the firmware's zone table was generated from (tools/gen_tz_table.py), and
only zones present in that compiled table are kept -- the device could not apply
any other name.

REPRESENTATION (format 1)
-------------------------
A region quadtree over the equirectangular world (lon -180..180, lat -90..90).
A cell is split into four (SW, SE, NW, NE) until it is homogeneous: wholly one
zone, wholly outside every zone ("none": open ocean), or -- from COAST_DEPTH on --
one zone plus ocean, which is resolved to that zone so coastal and near-shore
coordinates still resolve. At MAX_DEPTH a cell that still straddles a border
takes the zone covering most of its area. MAX_DEPTH 13 makes the finest cell
360/8192 deg by 180/8192 deg (4.9 km x 2.4 km at the equator, 4.4 x 2.4 km at
Uluru, 3.0 x 2.4 km in the Netherlands), which bounds the border error: a point
can only resolve to the wrong side of a zone border when it lies inside a
straddling finest cell, i.e. within roughly one cell (<= ~5 km) of that border.
COAST_DEPTH 11 means a point up to about one 20 km x 10 km cell beyond the
territorial-water line may still resolve to the coastal zone; open ocean is
"none" and the firmware then leaves the timezone unchanged.

The world is cut into 4^TILE_DEPTH tiles (32 x 32). Each tile is serialized in
pre-order as fixed-width symbols: 0 = internal node (four children follow),
k = leaf with the tile's k-th palette entry. A tile's palette is the handful of
zones it touches, so a symbol is usually 2-3 bits. Lookup walks one tile,
skipping sibling subtrees by counting (a few thousand symbols at most) -- this
runs at boot and on a location change only, never on a hot path.

Blob layout (little-endian):
   0  "TZG1"
   4  u8   format version (1)
   5  u8   TILE_DEPTH
   6  u8   MAX_DEPTH
   7  u8   COAST_DEPTH (informational)
   8  u16  zone count N
  10  u16  reserved (0)
  12  char[8]  source release, NUL padded
  20  u8[32]   source SHA-256
  52  u32  names offset   -> u16[N] string offsets (relative to the names
                             offset), then NUL-terminated names, sorted
  56  u32  tiles offset   -> u32[4^TILE_DEPTH] absolute tile offsets
  60  u32  blob size
  64  ...
  tile: u8 palette size k (1..255), u16[k] zone index (0xFFFF = none),
        then the MSB-first symbol bitstream, symbol width = bit_length(k).
A tile ends where the next one begins (the last one at the blob size).

Point -> cell: X = floor((lon + 180) / 360 * 2^MAX_DEPTH), Y likewise for
lat + 90 over 180, both clamped to 2^MAX_DEPTH - 1; C and Python evaluate the
same IEEE-754 double expression, so both land in the same cell.

EMITTED (never hand-edit; tests/test_tz_geo.py asserts consistency)
  components/timezone/tz_geo.bin   embedded into the firmware (EMBED_FILES)
  flash_gui/tz_geo_data.py         the same bytes, base64, for the frozen GUI

USAGE (the GIS dependency is generator-only and pinned for reproducibility)
  uv run --no-project --with shapely==2.2.0 python tools/gen_tz_geo.py
  uv run --no-project --with shapely==2.2.0 python tools/gen_tz_geo.py --check
The source zip is downloaded once into ~/.cache/ambyte-tz-geo/ (or pass
--source PATH) and is refused unless its SHA-256 matches the pin.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import math
import os
import struct
import sys
import urllib.request
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BIN_OUT = ROOT / "components/timezone/tz_geo.bin"
PY_OUT = ROOT / "flash_gui/tz_geo_data.py"
ZONE_TABLE_PY = ROOT / "flash_gui/tz_zone_table.py"

TBB_RELEASE = "2026c"
TBB_ASSET = "timezones.geojson.zip"
TBB_URL = ("https://github.com/evansiroky/timezone-boundary-builder/releases/"
           f"download/{TBB_RELEASE}/{TBB_ASSET}")
TBB_SHA256 = "7d3f0c5a33b6acd891335c0ad5ba767736b6914cb1a1d68c71921c17ce358948"
SHAPELY_PIN = "2.2.0"

FORMAT_VERSION = 1
MAX_DEPTH = 13
COAST_DEPTH = 11
TILE_DEPTH = 5
HEADER_SIZE = 64
NONE = 0xFFFF
# Pre-split depth for the parallel build; any value <= TILE_DEPTH gives the
# same tree (the split only distributes work).
SPLIT_DEPTH = 4


# -- source ------------------------------------------------------------------
def default_source() -> Path:
    cache = Path(os.environ.get("XDG_CACHE_HOME", Path.home() / ".cache"))
    return cache / "ambyte-tz-geo" / f"tbb-{TBB_RELEASE}-{TBB_ASSET}"


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def fetch_source(path: Path) -> Path:
    if not path.exists():
        path.parent.mkdir(parents=True, exist_ok=True)
        tmp = path.with_suffix(".part")
        print(f"downloading {TBB_URL}", file=sys.stderr)
        urllib.request.urlretrieve(TBB_URL, tmp)  # noqa: S310 - pinned https URL
        tmp.replace(path)
    digest = sha256_file(path)
    if digest != TBB_SHA256:
        raise SystemExit(f"{path}: SHA-256 {digest} does not match the pinned "
                         f"{TBB_RELEASE} release ({TBB_SHA256})")
    return path


def firmware_zone_names() -> frozenset[str]:
    """The compiled firmware table's names (flash_gui mirrors them exactly)."""
    namespace: dict = {}
    exec(compile(ZONE_TABLE_PY.read_text(encoding="utf-8"), str(ZONE_TABLE_PY), "exec"),
         namespace)
    return namespace["ZONES"]


# -- quadtree ----------------------------------------------------------------
# Worker-process globals (fork start method inherits them).
_GEOMS: list = []


def _cell(depth: int, x: int, y: int) -> tuple[float, float, float, float]:
    w = 360.0 / (1 << depth)
    h = 180.0 / (1 << depth)
    x0 = -180.0 + x * w
    y0 = -90.0 + y * h
    return x0, y0, x0 + w, y0 + h


def _build(depth: int, x: int, y: int, cands: list) -> int | list:
    """Tree node: int zone index, NONE, or [SW, SE, NW, NE] children."""
    import shapely
    x0, y0, x1, y1 = _cell(depth, x, y)
    area = (x1 - x0) * (y1 - y0)
    live = []
    for zone, geom in cands:
        clipped = shapely.clip_by_rect(geom, x0, y0, x1, y1)
        covered = clipped.area
        # Slivers below 1e-12 of the cell are clipping noise, not territory.
        if covered > area * 1e-12:
            live.append((zone, clipped, covered))
    if not live:
        return NONE
    if len(live) == 1:
        zone, _, covered = live[0]
        if covered >= area * (1 - 1e-9) or depth >= COAST_DEPTH:
            return zone
    if depth >= MAX_DEPTH:
        # Largest share wins; ties go to the lower index (sorted names) so the
        # output never depends on iteration order.
        return min(live, key=lambda t: (-t[2], t[0]))[0]
    kids = [_build(depth + 1, 2 * x + cx, 2 * y + cy, [(z, g) for z, g, _ in live])
            for cy in (0, 1) for cx in (0, 1)]
    return _merge(kids)


def _merge(kids: list) -> int | list:
    if all(isinstance(k, int) for k in kids) and len(set(kids)) == 1:
        return kids[0]
    return kids


def _work(job: tuple[int, int]) -> tuple[tuple[int, int], int | list]:
    import shapely
    x, y = job
    x0, y0, x1, y1 = _cell(SPLIT_DEPTH, x, y)
    cell = shapely.box(x0, y0, x1, y1)
    cands = [(i, shapely.clip_by_rect(g, x0, y0, x1, y1))
             for i, g in enumerate(_GEOMS) if g.intersects(cell)]
    return job, _build(SPLIT_DEPTH, x, y, cands)


def _assemble(depth: int, x: int, y: int, tiles: dict) -> int | list:
    if depth == SPLIT_DEPTH:
        return tiles[(x, y)]
    return _merge([_assemble(depth + 1, 2 * x + cx, 2 * y + cy, tiles)
                   for cy in (0, 1) for cx in (0, 1)])


def load_zones(source: Path, supported: frozenset[str]) -> tuple[list[str], list, list[str]]:
    import shapely
    from shapely.geometry import shape
    with zipfile.ZipFile(source) as archive:
        member = next(n for n in archive.namelist() if n.endswith("json"))
        data = json.loads(archive.read(member))
    features = sorted(data["features"], key=lambda f: f["properties"]["tzid"])
    names, geoms, dropped = [], [], []
    for feature in features:
        name = feature["properties"]["tzid"]
        if name not in supported:
            dropped.append(name)
            continue
        geom = shape(feature["geometry"])
        if not geom.is_valid:
            geom = shapely.make_valid(geom)
        shapely.prepare(geom)
        names.append(name)
        geoms.append(geom)
    return names, geoms, dropped


def build_tree(geoms: list) -> int | list:
    global _GEOMS
    import multiprocessing
    _GEOMS = geoms
    n = 1 << SPLIT_DEPTH
    jobs = [(x, y) for y in range(n) for x in range(n)]
    ctx = multiprocessing.get_context("fork")
    with ctx.Pool(max(1, (os.cpu_count() or 2) - 1)) as pool:
        tiles = dict(pool.imap_unordered(_work, jobs))
    return _assemble(0, 0, 0, tiles)


# -- serialization -----------------------------------------------------------
def _tile_root(tree: int | list, tx: int, ty: int) -> int | list:
    node = tree
    for level in range(TILE_DEPTH):
        if isinstance(node, int):
            return node
        shift = TILE_DEPTH - level - 1
        node = node[((ty >> shift) & 1) * 2 + ((tx >> shift) & 1)]
    return node


def _preorder(node: int | list, out: list[int | None]) -> None:
    if isinstance(node, int):
        out.append(node)
        return
    out.append(None)
    for kid in node:
        _preorder(kid, out)


def _encode_tile(node: int | list) -> bytes:
    seq: list[int | None] = []
    _preorder(node, seq)
    palette = sorted({v for v in seq if v is not None})
    if not 1 <= len(palette) <= 255:
        raise ValueError(f"tile palette of {len(palette)} zones does not fit format 1")
    width = len(palette).bit_length()
    index = {zone: i + 1 for i, zone in enumerate(palette)}
    acc = nbits = 0
    stream = bytearray()
    for v in seq:
        acc = (acc << width) | (0 if v is None else index[v])
        nbits += width
        while nbits >= 8:
            nbits -= 8
            stream.append((acc >> nbits) & 0xFF)
    if nbits:
        stream.append((acc << (8 - nbits)) & 0xFF)
    head = bytes([len(palette)]) + b"".join(struct.pack("<H", z) for z in palette)
    return head + bytes(stream)


def encode(names: list[str], tree: int | list, source_sha: str) -> bytes:
    names_blob = bytearray()
    offsets = []
    strings = bytearray()
    for name in names:
        offsets.append(2 * len(names) + len(strings))
        strings += name.encode("ascii") + b"\0"
    for off in offsets:
        names_blob += struct.pack("<H", off)
    names_blob += strings
    if len(names_blob) > 0xFFFF:
        raise ValueError("name table exceeds u16 offsets")

    n_tiles = 1 << (2 * TILE_DEPTH)
    names_off = HEADER_SIZE
    tiles_off = names_off + len(names_blob)
    tiles_off += (-tiles_off) % 4
    data_off = tiles_off + 4 * n_tiles
    tile_offsets = []
    tile_data = bytearray()
    side = 1 << TILE_DEPTH
    for ty in range(side):
        for tx in range(side):
            tile_offsets.append(data_off + len(tile_data))
            tile_data += _encode_tile(_tile_root(tree, tx, ty))
    size = data_off + len(tile_data)

    out = bytearray(b"TZG1")
    out += struct.pack("<BBBBHH", FORMAT_VERSION, TILE_DEPTH, MAX_DEPTH, COAST_DEPTH,
                       len(names), 0)
    out += TBB_RELEASE.encode("ascii").ljust(8, b"\0")
    out += bytes.fromhex(source_sha)
    out += struct.pack("<III", names_off, tiles_off, size)
    assert len(out) == HEADER_SIZE
    out += names_blob
    out += b"\0" * (tiles_off - len(out))
    out += b"".join(struct.pack("<I", off) for off in tile_offsets)
    out += tile_data
    assert len(out) == size
    return bytes(out)


def render_py(blob: bytes) -> str:
    digest = hashlib.sha256(blob).hexdigest()
    encoded = base64.b64encode(blob).decode("ascii")
    lines = [
        "# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute",
        "# SPDX-License-Identifier: GPL-3.0-only",
        "",
        '"""Coordinate -> IANA timezone quadtree (generated).',
        "",
        "GENERATED FILE -- DO NOT EDIT BY HAND.",
        "Regenerate with `python tools/gen_tz_geo.py` (that script documents the",
        "source, the format and the accuracy). Byte-identical to",
        "components/timezone/tz_geo.bin, which the firmware embeds; kept as a module",
        "so the frozen flash_gui executable carries it without a data-file spec.",
        "",
        f"Source: timezone-boundary-builder {TBB_RELEASE} ({TBB_ASSET}, ODbL),",
        f"sha256 {TBB_SHA256}.",
        '"""',
        "",
        "from __future__ import annotations",
        "",
        f'SOURCE_RELEASE = "{TBB_RELEASE}"',
        f'BLOB_SHA256 = "{digest}"',
        "",
        "BLOB_B64 = (",
    ]
    lines += [f'    "{encoded[i:i + 76]}"' for i in range(0, len(encoded), 76)]
    lines += [")", ""]
    return "\n".join(lines)


def lookup(blob: bytes, lat: float, lon: float) -> str | None:
    """Reference reader used by the generator self-check (flash_gui/tz_geo.py
    is the shipped one; tests assert they agree)."""
    sys.path.insert(0, str(ROOT))
    from flash_gui.tz_geo import _lookup  # noqa: PLC0415
    return _lookup(blob, lat, lon)


def verify_against_polygons(blob: bytes, names: list[str], geoms: list, samples: int) -> None:
    """Sample random points inside zones; report how far misses are from a border."""
    import random
    import shapely
    from shapely.geometry import Point
    rng = random.Random(1)
    tree = shapely.STRtree(geoms)
    misses = []
    tested = 0
    while tested < samples:
        lat = math.degrees(math.asin(rng.uniform(-1, 1)))
        lon = rng.uniform(-180, 180)
        p = Point(lon, lat)
        hits = [i for i in tree.query(p, predicate="intersects")]
        if not hits:
            continue
        tested += 1
        want = names[hits[0]]
        got = lookup(blob, lat, lon)
        if got != want:
            border = geoms[hits[0]].boundary.distance(p)
            misses.append((border * 111.0, lat, lon, want, got))
    misses.sort(reverse=True)
    print(f"verify: {tested} land points, {len(misses)} resolved differently "
          f"({100.0 * len(misses) / tested:.3f} %)")
    for km, lat, lon, want, got in misses[:5]:
        print(f"  farthest: {km:.2f} km-ish from border at ({lat:.4f},{lon:.4f}) "
              f"{want} -> {got}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--source", type=Path, default=None,
                        help=f"path to {TBB_ASSET} (default: download into the cache)")
    parser.add_argument("--check", action="store_true",
                        help="verify the checked-in artefacts match a fresh build")
    parser.add_argument("--verify", type=int, default=0, metavar="N",
                        help="also compare N random land points against the polygons")
    args = parser.parse_args(argv)

    try:
        import shapely
    except ImportError:
        print(f"error: needs shapely=={SHAPELY_PIN} (uv run --no-project --with "
              f"shapely=={SHAPELY_PIN} python tools/gen_tz_geo.py)", file=sys.stderr)
        return 2
    if shapely.__version__ != SHAPELY_PIN:
        print(f"warning: shapely {shapely.__version__} != pinned {SHAPELY_PIN}; "
              "border cells may resolve differently", file=sys.stderr)

    source = fetch_source(args.source or default_source())
    names, geoms, dropped = load_zones(source, firmware_zone_names())
    for name in dropped:
        print(f"warning: {name} is not in the firmware zone table; dropped", file=sys.stderr)
    tree = build_tree(geoms)
    blob = encode(names, tree, TBB_SHA256)
    artefacts = {BIN_OUT: blob, PY_OUT: render_py(blob).encode("utf-8")}

    if args.verify:
        verify_against_polygons(blob, names, geoms, args.verify)
    stale = [p for p, data in artefacts.items() if not p.exists() or p.read_bytes() != data]
    if args.check:
        for path in stale:
            print(f"out of date: {path.relative_to(ROOT)}", file=sys.stderr)
        return 1 if stale else 0
    for path, data in artefacts.items():
        path.write_bytes(data)
    print(f"wrote {len(names)} zones, {len(blob)} bytes "
          f"(depth {MAX_DEPTH}, coast {COAST_DEPTH}, tiles {1 << TILE_DEPTH}^2)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
