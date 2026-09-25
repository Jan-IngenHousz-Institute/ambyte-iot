"""Compile the storage-harness driver against the PRODUCTION event store.

HEAD: components/event_log/{event_log,evq_index,evq_render}.c at the working
tree. Baseline: `git show <rev>:components/event_log/event_log.c` (+ its header)
with ONLY the absolute path literals rewritten ("/evstore" → "./evstore",
"/sdcard" → "./sdcard") so it runs inside a temp state dir; the rewrite is
returned for the evidence. Both are built with clang ASan+UBSan and the media
shim forced into the production translation units.
"""
from __future__ import annotations

import hashlib
import os
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HOST = ROOT / "tests" / "evq_host"

SAN = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
BASE_FLAGS = ["-std=gnu11", "-g", "-O1", "-Wall", "-Wextra", "-Werror", *SAN]

# Host test sizes (production values are pinned by check_constants.py).
DEFAULT_TUNING = {
    "EVLOG_ROTATE_BYTES": 65536,
    "EVQ_SD_RESERVE_BYTES": 8 * 1024 * 1024,
}


def cc() -> str:
    c = os.environ.get("CC") or shutil.which("clang")
    if not c or os.path.basename(c) == "cc":
        c = shutil.which("clang") or "clang"
    return c


def _run(cmd: list[str], cwd: Path | None = None) -> None:
    r = subprocess.run(cmd, cwd=cwd or ROOT, capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"compile failed: {' '.join(cmd)}\n{r.stdout}\n{r.stderr}")


def _defines(tuning: dict[str, int] | None) -> list[str]:
    d = [f'-DEVSTORE_MOUNT="./evstore"', f'-DSD_MOUNT_POINT="./sdcard"', "-DEVQ_HOST_FAULTS"]
    for k, v in {**DEFAULT_TUNING, **(tuning or {})}.items():
        d.append(f"-D{k}={v}")
    return d


def build_head(out_dir: Path, tuning: dict[str, int] | None = None, name: str = "evq_driver_head") -> Path:
    out_dir.mkdir(parents=True, exist_ok=True)
    objdir = out_dir / (name + ".o.d")
    objdir.mkdir(exist_ok=True)
    inc = [f"-I{HOST / 'stubs'}", f"-I{ROOT / 'components/event_log/include'}", f"-I{ROOT / 'components/event_log'}",
           f"-I{ROOT / 'components/domain/include'}", f"-I{ROOT / 'components/sd_card'}", f"-I{HOST}"]
    flags = BASE_FLAGS + inc + _defines(tuning)
    objs = []
    for src in ["components/event_log/event_log.c", "components/event_log/evq_index.c"]:
        o = objdir / (Path(src).stem + ".o")
        _run([cc(), *flags, "-include", str(HOST / "fsshim.h"), "-c", str(ROOT / src), "-o", str(o)])
        objs.append(o)
    for src in ["components/event_log/evq_render.c", "tests/evq_host/stubs.c", "tests/evq_host/fsshim.c",
                "tests/evq_host/sha256.c", "tests/evq_host/evq_driver.c"]:
        o = objdir / (Path(src).stem + ".o")
        _run([cc(), *flags, "-c", str(ROOT / src), "-o", str(o)])
        objs.append(o)
    exe = out_dir / name
    _run([cc(), *SAN, *map(str, objs), "-o", str(exe)])
    return exe


BASELINE_REWRITES = [('"/evstore"', '"./evstore"'), ('"/sdcard/', '"./sdcard/')]


def baseline_sources(rev: str, dst: Path) -> dict:
    """Extract a baseline event_log (+header) and apply only the path rewrite."""
    dst.mkdir(parents=True, exist_ok=True)
    (dst / "include").mkdir(exist_ok=True)
    rewrites = []
    for rel, out in [("components/event_log/event_log.c", dst / "event_log.c"),
                     ("components/event_log/include/event_log.h", dst / "include" / "event_log.h")]:
        text = subprocess.run(["git", "show", f"{rev}:{rel}"], cwd=ROOT, capture_output=True, text=True, check=True).stdout
        orig_sha = hashlib.sha256(text.encode()).hexdigest()
        new = text
        count = 0
        for a, b in BASELINE_REWRITES:
            count += new.count(a)
            new = new.replace(a, b)
        out.write_text(new)
        rewrites.append({"file": rel, "rev": rev, "orig_sha256": orig_sha, "rewritten_literals": count,
                         "rules": BASELINE_REWRITES})
    return {"rev": rev, "rewrites": rewrites}


def build_baseline(rev: str, out_dir: Path, name: str | None = None) -> tuple[Path, dict]:
    name = name or f"evq_driver_{rev.replace('.', '_')}"
    src = out_dir / (name + ".src")
    info = baseline_sources(rev, src)
    objdir = out_dir / (name + ".o.d")
    objdir.mkdir(parents=True, exist_ok=True)
    inc = [f"-I{HOST / 'stubs'}", f"-I{src / 'include'}", f"-I{ROOT / 'components/domain/include'}",
           f"-I{ROOT / 'components/sd_card'}", f"-I{HOST}"]
    # Baseline code predates -Werror cleanliness under these stubs; warnings are
    # reported, not fatal. Sanitizers stay on.
    flags = ["-std=gnu11", "-g", "-O1", "-Wall", *SAN] + inc + [
        '-DEVSTORE_MOUNT_UNUSED=1', "-DEVQ_BASELINE"]
    objs = []
    o = objdir / "event_log.o"
    _run([cc(), *flags, "-include", str(HOST / "fsshim.h"), "-c", str(src / "event_log.c"), "-o", str(o)])
    objs.append(o)
    for s in ["tests/evq_host/stubs.c", "tests/evq_host/fsshim.c", "tests/evq_host/sha256.c", "tests/evq_host/evq_driver.c"]:
        o = objdir / (Path(s).stem + ".o")
        _run([cc(), *flags, "-c", str(ROOT / s), "-o", str(o)])
        objs.append(o)
    # the baseline also compiles evlog_inventory.c in its component; not needed here.
    exe = out_dir / name
    _run([cc(), *SAN, *map(str, objs), "-o", str(exe)])
    return exe, info
