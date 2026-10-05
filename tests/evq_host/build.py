"""Compile the storage-harness driver against the PRODUCTION event store.

HEAD: components/event_log/{event_log,evq_index,evq_render}.c at the working
tree. Baseline: `git show <rev>:components/event_log/event_log.c` (+ its header)
with ONLY the absolute path literals rewritten ("/evstore" → "./evstore",
"/sdcard" → "./sdcard") so it runs inside a temp state dir; the rewrite is
returned for the evidence. Both are built with clang ASan+UBSan and the media
shim forced into the production translation units.

Full baseline (`build_rev_full`, Sprint 1 red/green): a revision that already
has the indexed store (evq_index.c, e.g. e1ca6ee) is built exactly like HEAD —
same driver mode, same defines — from `git show <rev>:` of every file under
components/event_log/ plus components/sd_card/sd_card.h and
components/domain/include/, with the same path-literal rewrite rules.

Every variant links the pure sd_diag core (components/sd_card/sd_diag_core.c,
working tree) because the host stubs implement the sd_diag device API over it;
a baseline event_log simply never calls it.
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


SD_DIAG_CORE = ROOT / "components" / "sd_card" / "sd_diag_core.c"
HARNESS_SOURCES = ["stubs.c", "fsshim.c", "sha256.c", "evq_driver.c"]


def _health_defines(header: Path) -> list[str]:
    """Driver feature switches derived from the event_log.h actually compiled."""
    text = header.read_text()
    return ["-DEVQ_HEALTH_SDX=1"] if "sd_rename_ambiguous" in text else []


def _build_indexed(out_dir: Path, name: str, el_dir: Path, inc_dirs: list[Path], tuning: dict[str, int] | None,
                   strict: bool, variant: str = "head") -> Path:
    """Build one driver against an indexed-store event_log tree `el_dir`
    (event_log.c, evq_index.c, evq_render.c, include/)."""
    out_dir.mkdir(parents=True, exist_ok=True)
    objdir = out_dir / (name + ".o.d")
    objdir.mkdir(exist_ok=True)
    inc = [f"-I{HOST / 'stubs'}", f"-I{el_dir / 'include'}", f"-I{el_dir}", *[f"-I{d}" for d in inc_dirs],
           f"-I{ROOT / 'components/sd_card'}", f"-I{HOST}"]
    warn = BASE_FLAGS if strict else ["-std=gnu11", "-g", "-O1", "-Wall", *SAN]
    flags = warn + inc + _defines(tuning) + _health_defines(el_dir / "include" / "event_log.h") + \
        [f'-DEVQ_VARIANT="{variant}"']
    objs = []
    for src in [el_dir / "event_log.c", el_dir / "evq_index.c"]:
        o = objdir / (src.stem + ".o")
        _run([cc(), *flags, "-include", str(HOST / "fsshim.h"), "-c", str(src), "-o", str(o)])
        objs.append(o)
    for src in [el_dir / "evq_render.c", SD_DIAG_CORE, *[HOST / h for h in HARNESS_SOURCES]]:
        o = objdir / (src.stem + ".o")
        _run([cc(), *flags, "-c", str(src), "-o", str(o)])
        objs.append(o)
    exe = out_dir / name
    _run([cc(), *SAN, *map(str, objs), "-o", str(exe)])
    return exe


def build_head(out_dir: Path, tuning: dict[str, int] | None = None, name: str = "evq_driver_head") -> Path:
    return _build_indexed(out_dir, name, ROOT / "components" / "event_log", [ROOT / "components/domain/include"],
                          tuning, strict=True)


def _git_show(rev: str, rel: str) -> str:
    return subprocess.run(["git", "show", f"{rev}:{rel}"], cwd=ROOT, capture_output=True, text=True, check=True).stdout


def rev_full_sources(rev: str, dst: Path) -> dict:
    """Extract every file of components/event_log/ (+ sd_card.h, domain
    includes) at `rev` and apply only BASELINE_REWRITES. Returns the rewrite
    record (per-file original SHA-256 and literal count) for the evidence."""
    ls = subprocess.run(["git", "ls-tree", "-r", "--name-only", rev, "--", "components/event_log",
                         "components/domain/include", "components/sd_card/sd_card.h"],
                        cwd=ROOT, capture_output=True, text=True, check=True).stdout.split()
    if "components/event_log/evq_index.c" not in ls:
        raise RuntimeError(f"{rev} has no components/event_log/evq_index.c: use build_baseline for pre-index revisions")
    commit = subprocess.run(["git", "rev-parse", f"{rev}^{{commit}}"], cwd=ROOT, capture_output=True, text=True,
                            check=True).stdout.strip()
    rewrites = []
    for rel in ls:
        if not rel.endswith((".c", ".h", ".inc")):
            continue
        text = _git_show(rev, rel)
        orig_sha = hashlib.sha256(text.encode()).hexdigest()
        new = text
        count = 0
        for a, b in BASELINE_REWRITES:
            count += new.count(a)
            new = new.replace(a, b)
        out = dst / rel
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(new)
        rewrites.append({"file": rel, "orig_sha256": orig_sha, "rewritten_literals": count})
    return {"rev": rev, "commit": commit, "mode": "full", "rules": BASELINE_REWRITES, "rewrites": rewrites}


def build_rev_full(rev: str, out_dir: Path, name: str | None = None,
                   tuning: dict[str, int] | None = None) -> tuple[Path, dict]:
    """HEAD-equivalent driver built from `git show <rev>:` sources (see module doc)."""
    name = name or f"evq_driver_full_{rev.replace('.', '_')}"
    src = out_dir / (name + ".src")
    if src.exists():
        shutil.rmtree(src)
    info = rev_full_sources(rev, src)
    exe = _build_indexed(out_dir, name, src / "components" / "event_log", [src / "components/sd_card",
                                                                             src / "components/domain/include"],
                         tuning, strict=False, variant=f"rev-{rev}")
    return exe, info


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
        '-DEVSTORE_MOUNT_UNUSED=1', "-DEVQ_BASELINE", f'-DEVQ_VARIANT="legacy-{rev}"']
    objs = []
    o = objdir / "event_log.o"
    _run([cc(), *flags, "-include", str(HOST / "fsshim.h"), "-c", str(src / "event_log.c"), "-o", str(o)])
    objs.append(o)
    for s in [SD_DIAG_CORE, *[HOST / h for h in HARNESS_SOURCES]]:
        o = objdir / (Path(s).stem + ".o")
        _run([cc(), *flags, "-c", str(s), "-o", str(o)])
        objs.append(o)
    # the baseline also compiles evlog_inventory.c in its component; not needed here.
    exe = out_dir / name
    _run([cc(), *SAN, *map(str, objs), "-o", str(exe)])
    return exe, info
