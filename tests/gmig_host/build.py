"""Build the two G-MIG executables from git-pinned sources.

  old: v1.11.0 (commit 8af0c76 on main, pinned by id): `git show 8af0c76:` of
       components/event_log/{event_log.c,include/event_log.h},
       components/domain/include/*, components/sd_card/sd_card.h
  new: the candidate (default C2 = 6fcbeba, rebuilt from its committed fixture by
       tests/pinned_revs.py; GMIG_C2_REV=<rev> for another commit,
       GMIG_C2_REV=WORKTREE for the working tree): every file under
       components/event_log/ plus components/sd_card/{sd_card.h,sd_diag.h,
       sd_diag_core.c} and components/domain/include/*.

Harness files (tests/evq_host media shim fsshim.c/.h, stubs.c, sha256.c/.h,
evq_host.h, stubs/*) are taken from the SAME candidate revision, so a
concurrent edit of the working tree cannot change what is proven. Both
executables link the same harness objects and the G-MIG NVS model
(nvs_model.c); only the event store (and its own headers) differ.

The ONLY source edits are the path-literal rewrites the evq_host harness
already uses ('"/evstore"' -> '"./evstore"', '"/sdcard/' -> '"./sdcard/') so
the firmware runs inside a temp state dir; each rewrite is counted and the
original file SHA-256 recorded in build_info.json. Production tuning
constants are NOT overridden (EVLOG_ROTATE_BYTES 256 KiB, archive burst 1000,
pressure 25 %/40 %, SD reserve 64 MiB): unlike tests/evq_host this build is
about real on-device behaviour, not accelerated scenarios.

stubs.c's own committed-only NVS stub is compiled out of the way by renaming
its nvs_* symbols (-Dnvs_open=evqstub_nvs_open ...); nvs_model.c provides the
real ones for every translation unit.

clang (CC, default clang), ASan+UBSan, -fno-sanitize-recover.
"""
from __future__ import annotations

import hashlib
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HERE = ROOT / "tests" / "gmig_host"
sys.path.insert(0, str(ROOT / "tests"))
import pinned_revs  # noqa: E402
OLD_REV = pinned_revs.V1_11_0      # by commit id: a tag name is a ref that can be absent or move
DEFAULT_NEW_REV = pinned_revs.resolve(pinned_revs.C2)   # C2 rebuilt from its fixture: a tree id

SAN = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
REWRITES = [('"/evstore"', '"./evstore"'), ('"/sdcard/', '"./sdcard/')]
NVS_RENAMES = ["nvs_open", "nvs_close", "nvs_get_u32", "nvs_set_u32", "nvs_get_u64", "nvs_set_u64",
               "nvs_get_blob", "nvs_set_blob", "nvs_commit"]
HARNESS = ["tests/evq_host/stubs.c", "tests/evq_host/fsshim.c", "tests/evq_host/fsshim.h",
           "tests/evq_host/sha256.c", "tests/evq_host/sha256.h", "tests/evq_host/evq_host.h"]


def cc() -> str:
    c = os.environ.get("CC") or "clang"
    if os.path.basename(c) in ("cc", "gcc"):
        c = shutil.which("clang") or c      # ASan+UBSan flags below are clang's
    return c


def tmp_root() -> Path:
    base = os.environ.get("GMIG_TMPDIR") or os.environ.get("TMPDIR") or "/tmp"
    p = Path(base) / "gmig_host"
    p.mkdir(parents=True, exist_ok=True)
    return p


def _git(*args: str) -> str:
    return subprocess.run(["git", *args], cwd=ROOT, capture_output=True, text=True, check=True,
                          env=pinned_revs.env()).stdout


def _commit(rev: str) -> str:
    """Commit id, or the tree id for a rebuilt fixture baseline (content-addressed either way)."""
    r = subprocess.run(["git", "rev-parse", "--verify", "--quiet", f"{rev}^{{commit}}"], cwd=ROOT,
                       capture_output=True, text=True, env=pinned_revs.env())
    return r.stdout.strip() if r.returncode == 0 else _git("rev-parse", f"{rev}^{{tree}}").strip()


def _ls(rev: str, *paths: str) -> list[str]:
    return [p for p in _git("ls-tree", "-r", "--name-only", rev, "--", *paths).split() if p]


def _read(rev: str, rel: str) -> str:
    if rev == "WORKTREE":
        return (ROOT / rel).read_text()
    return _git("show", f"{rev}:{rel}")


def _materialize(rev: str, rels: list[str], dst: Path, rewrite_prefix: str) -> list[dict]:
    rec = []
    for rel in rels:
        text = _read(rev, rel)
        orig = hashlib.sha256(text.encode()).hexdigest()
        n = 0
        if rel.startswith(rewrite_prefix):
            for a, b in REWRITES:
                n += text.count(a)
                text = text.replace(a, b)
        out = dst / rel
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(text)
        rec.append({"file": rel, "orig_sha256": orig, "rewritten_literals": n})
    return rec


def _run(cmd: list[str], cwd: Path | None = None) -> None:
    r = subprocess.run(cmd, cwd=cwd or ROOT, capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"compile failed: {' '.join(cmd)}\n{r.stdout}\n{r.stderr}")


def new_rev() -> str:
    return os.environ.get("GMIG_C2_REV", DEFAULT_NEW_REV)


def _new_files(rev: str) -> list[str]:
    if rev == "WORKTREE":
        ev = [str(p.relative_to(ROOT)) for p in (ROOT / "components/event_log").rglob("*")
              if p.is_file() and p.suffix in (".c", ".h")]
        dom = [str(p.relative_to(ROOT)) for p in (ROOT / "components/domain/include").glob("*.h")]
        stubs = [str(p.relative_to(ROOT)) for p in (ROOT / "tests/evq_host/stubs").rglob("*.h")]
    else:
        ev = [p for p in _ls(rev, "components/event_log") if p.endswith((".c", ".h"))]
        dom = _ls(rev, "components/domain/include")
        stubs = _ls(rev, "tests/evq_host/stubs")
    sd = ["components/sd_card/sd_card.h", "components/sd_card/sd_diag.h", "components/sd_card/sd_diag_core.c"]
    return sorted(set(ev + dom + sd + stubs + HARNESS))


def _old_files() -> list[str]:
    return ["components/event_log/event_log.c", "components/event_log/include/event_log.h",
            "components/sd_card/sd_card.h", *_ls(OLD_REV, "components/domain/include")]


def _key(rev: str) -> str:
    h = hashlib.sha256()
    for f in sorted(HERE.glob("*.c")) + sorted((HERE / "stubs").glob("*.h")) + [Path(__file__)]:
        h.update(f.read_bytes())
    h.update(cc().encode())
    if rev == "WORKTREE":
        for rel in _new_files(rev):
            h.update((ROOT / rel).read_bytes())
    else:
        h.update(_commit(rev).encode())
    h.update(_commit(OLD_REV).encode())
    return h.hexdigest()[:16]


def build(force: bool = False) -> dict:
    """Returns {"old": exe, "new": exe, "info": build_info} (cached by content key)."""
    rev = new_rev()
    out = tmp_root() / f"build-{_key(rev)}"
    info_p = out / "build_info.json"
    if info_p.exists() and not force:
        info = json.loads(info_p.read_text())
        if Path(info["old_exe"]).exists() and Path(info["new_exe"]).exists():
            return {"old": Path(info["old_exe"]), "new": Path(info["new_exe"]), "info": info}
    if out.exists():
        shutil.rmtree(out)
    newsrc, oldsrc = out / "src-new", out / "src-old"
    new_rec = _materialize(rev, _new_files(rev), newsrc, "components/event_log/")
    old_rec = _materialize(OLD_REV, _old_files(), oldsrc, "components/event_log/")

    warn = ["-std=gnu11", "-g", "-O1", "-Wall", "-Wextra", *SAN]
    ren = [f"-D{n}=evqstub_{n}" for n in NVS_RENAMES]
    hs = newsrc / "tests/evq_host"
    common_inc = [f"-I{HERE / 'stubs'}", f"-I{hs / 'stubs'}", f"-I{hs}"]

    def objs_for(variant: str, ev_srcs: list[Path], ev_inc: list[Path], ev_defs: list[str]) -> Path:
        od = out / f"obj-{variant}"
        od.mkdir(parents=True)
        inc = common_inc + [f"-I{d}" for d in ev_inc]
        vdef = [f'-DGMIG_VARIANT="{variant}"']
        objs = []
        for s in ev_srcs:                                      # production TUs: forced media shim
            o = od / (s.stem + ".o")
            fl = [*warn, "-Wno-unused-parameter"] if variant == "v1.11.0" else [*warn, "-Werror"]
            _run([cc(), *fl, *inc, *ev_defs, *vdef, "-include", str(hs / "fsshim.h"), "-c", str(s), "-o", str(o)])
            objs.append(o)
        # harness TUs (plain libc)
        for s, extra in [(hs / "stubs.c", ren), (hs / "fsshim.c", []), (hs / "sha256.c", []),
                         (newsrc / "components/sd_card/sd_diag_core.c", []),
                         (HERE / "nvs_model.c", []), (HERE / "gmig_driver.c", [])]:
            o = od / (s.stem + ".o")
            sinc = inc + [f"-I{newsrc / 'components/sd_card'}"] if s.name in ("stubs.c", "sd_diag_core.c") else inc
            fl = [*warn, "-Werror"] if s.parent == HERE else warn
            _run([cc(), *fl, *sinc, *ev_defs, *vdef, *extra, "-c", str(s), "-o", str(o)])
            objs.append(o)
        exe = out / f"gmig_{variant.replace('.', '_')}"
        _run([cc(), *SAN, *map(str, objs), "-o", str(exe)])
        return exe

    ne = newsrc / "components/event_log"
    new_exe = objs_for("candidate",
                       [ne / "event_log.c", ne / "evq_index.c", ne / "evq_render.c"],
                       [ne / "include", ne, newsrc / "components/domain/include", newsrc / "components/sd_card"],
                       ['-DEVSTORE_MOUNT="./evstore"', '-DSD_MOUNT_POINT="./sdcard"', "-DEVQ_HOST_FAULTS"])
    oe = oldsrc / "components/event_log"
    old_exe = objs_for("v1.11.0", [oe / "event_log.c"],
                       [oe / "include", oldsrc / "components/domain/include", oldsrc / "components/sd_card"],
                       ["-DGMIG_V1110"])
    info = {
        "old_rev": "v1.11.0", "old_commit": _commit(OLD_REV),
        "new_rev": rev, "new_commit": "WORKTREE" if rev == "WORKTREE" else _commit(rev),
        "new_pin": ({"original": pinned_revs.C2.original, "resigned": pinned_revs.C2.resigned,
                     "tree": pinned_revs.C2.tree, "fixture": pinned_revs.C2.patch,
                     "patch_sha256": pinned_revs.C2.patch_sha256} if rev == DEFAULT_NEW_REV else None),
        "cc": cc(), "old_exe": str(old_exe), "new_exe": str(new_exe),
        "rewrite_rules": REWRITES, "old_sources": old_rec, "new_sources": new_rec,
        "tuning_overrides": {},
    }
    info_p.write_text(json.dumps(info, indent=1))
    return {"old": old_exe, "new": new_exe, "info": info}


if __name__ == "__main__":
    r = build(force=True)
    print(json.dumps({k: str(v) for k, v in r.items() if k != "info"}, indent=1))
