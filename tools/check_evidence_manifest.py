#!/usr/bin/env python3
"""Evidence-provenance manifest for the SD write-integrity sprint (contract §5.1, C-00b).

Why this exists: sprint evidence used to be judged by file mtimes and "the ELF hash
matched last time". Both lie - a stale build dir, a reused run directory or a file
touched after the fact passes an mtime check, and ESP-IDF builds are not bit-for-bit
reproducible, so cross-run hash equality is not a freshness proof either. Freshness is
instead proved *by construction* inside one invocation of tools/sprint1_evidence.sh:

  * the run directory `$OUT` did not exist before the `init` step (non-`-p` mkdir, and
    the init record hashes it as an EMPTY directory);
  * every compared input was listed as an output of an earlier successful step in the
    same manifest;
  * every compared input still has the SHA-256 recorded when it was produced.

Modes (mutually exclusive):
  --record   append one step record (outputs hashed) to the manifest
  --consume  refuse to run a comparison unless all its inputs are provably fresh
  --verify   static check of the evidence script + whole-manifest self-check
  --compare-runs  informational cross-invocation ELF hash comparison (always exit 0)

Stdlib only: this runs inside the evidence script on whatever python the host has.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
from pathlib import Path

# Exit codes. Distinct values so the evidence log says *why* a step failed:
#   3 = a zero-exit step did not produce an output it promised (the step must FAIL -
#       a comparison that later consumes a missing file would otherwise be vacuous);
#   4 = --consume refused an input (the wrapped comparison was NOT run).
EXIT_MISSING_OUTPUT = 3
EXIT_CONSUME_REFUSED = 4

EMPTY_SHA256 = hashlib.sha256(b"").hexdigest()
_CHUNK = 1 << 20


# --------------------------------------------------------------------------- hashing

def sha256_file(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            b = f.read(_CHUNK)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


def dir_listing(path: str) -> str:
    """Sorted `relpath\\tsha256\\n` listing of every regular file under `path`.

    A directory hash is the SHA-256 of this text (contract §5.1), so ANY file added,
    removed, renamed or changed below the directory changes the hash - that is what
    lets `hil_wrap` consume the whole `$HIL` object directory by one hash (C-36
    amendment). Symlinks are not followed (a link could point outside `$OUT`); a link
    is listed with the hash of `symlink:<target>` so adding/retargeting one still
    changes the directory hash. An empty directory lists as "" (EMPTY_SHA256), which
    is exactly what the `init` record must show for a fresh `$OUT`.
    """
    entries: list[str] = []
    for root, dirs, files in os.walk(path, followlinks=False):
        # Directory symlinks appear in `dirs`; os.walk won't descend (followlinks=False)
        # but we still want them to affect the hash.
        for name in list(dirs) + files:
            full = os.path.join(root, name)
            rel = os.path.relpath(full, path).replace(os.sep, "/")
            if os.path.islink(full):
                target = os.readlink(full)
                entries.append(f"{rel}\t{hashlib.sha256(('symlink:' + target).encode()).hexdigest()}\n")
            elif name in files and os.path.isfile(full):
                entries.append(f"{rel}\t{sha256_file(full)}\n")
    entries.sort()
    return "".join(entries)


def sha256_dir(path: str) -> str:
    return hashlib.sha256(dir_listing(path).encode("utf-8")).hexdigest()


def hash_path(path: str) -> tuple[str, str]:
    """Return (kind, sha256) for an existing path. Raises FileNotFoundError otherwise."""
    if os.path.isdir(path) and not os.path.islink(path):
        return "dir", sha256_dir(path)
    if os.path.isfile(path):
        return "file", sha256_file(path)
    raise FileNotFoundError(path)


def norm(path: str) -> str:
    """Canonical spelling used for 'exact path' matching between record and consume.

    abspath (not realpath): the record keeps the spelling the script used; symlink
    games are caught separately by the realpath-inside-$OUT check.
    """
    return os.path.normpath(os.path.abspath(path))


def inside(path: str, outdir: str) -> bool:
    rp, ro = os.path.realpath(path), os.path.realpath(outdir)
    try:
        return os.path.commonpath([rp, ro]) == ro
    except ValueError:
        return False


# --------------------------------------------------------------------------- manifest io

def load_manifest(path: str) -> list[dict]:
    recs: list[dict] = []
    with open(path, encoding="utf-8") as f:
        for i, line in enumerate(f, 1):
            line = line.strip()
            if not line:
                continue
            try:
                rec = json.loads(line)
            except json.JSONDecodeError as e:
                raise ValueError(f"{path}:{i}: malformed JSON line ({e})") from None
            if not isinstance(rec, dict):
                raise ValueError(f"{path}:{i}: record is not a JSON object")
            recs.append(rec)
    return recs


def append_record(path: str, rec: dict) -> None:
    # One line, one write, fsync: the manifest is the evidence; a torn last line would
    # make --verify fail loudly rather than pass on a partial record.
    line = json.dumps(rec, sort_keys=True, separators=(",", ":")) + "\n"
    with open(path, "a", encoding="utf-8") as f:
        f.write(line)
        f.flush()
        os.fsync(f.fileno())


# --------------------------------------------------------------------------- --record

def _first_line(path: str) -> str:
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            line = f.readline().strip()
        return line or "unknown"
    except OSError:
        return "unknown"


def _pio_version() -> str:
    try:
        r = subprocess.run(["pio", "--version"], capture_output=True, text=True, timeout=60)
        out = r.stdout.strip()
        return out if r.returncode == 0 and out else "unknown"
    except (OSError, subprocess.SubprocessError):
        return "unknown"


def _platform_version() -> str:
    p = Path.home() / ".platformio" / "platforms" / "espressif32" / "platform.json"
    try:
        v = json.loads(p.read_text(encoding="utf-8")).get("version")
        return str(v) if v else "unknown"
    except (OSError, ValueError, AttributeError):
        return "unknown"


def cmd_record(a: argparse.Namespace) -> int:
    manifest = a.manifest
    outdir = os.path.dirname(os.path.abspath(manifest))
    paths = [p for p in a.outputs.split(",") if p.strip()] if a.outputs else []
    outputs: list[dict] = []
    missing: list[str] = []
    # Hash BEFORE touching the manifest: the init step lists $OUT itself, and its
    # listing must be empty - the manifest file must not exist yet when it is hashed.
    for p in paths:
        try:
            kind, digest = hash_path(p)
        except FileNotFoundError:
            missing.append(norm(p))
            continue
        outputs.append({"path": norm(p), "kind": kind, "sha256": digest})

    rec: dict = {"type": "step", "name": a.name, "start": a.start, "end": a.end,
                 "exit": a.exit, "outputs": outputs}
    if missing:
        rec["missing"] = missing

    elf = next((o for o in outputs if o["path"].endswith("firmware.elf") and o["kind"] == "file"), None)
    if elf is not None or any(m.endswith("firmware.elf") for m in missing):
        # Firmware provenance (§5.1): which sources, which toolchain. `commit` comes
        # from the identity files the same invocation wrote - the baseline build names
        # base.id, everything else s1.id - never from a live `git rev-parse` here, which
        # could observe a checkout that moved after the build.
        bin_path = os.path.join(os.path.dirname(elf["path"]), "firmware.bin") if elf else None
        rec["firmware"] = {
            "elf_sha256": elf["sha256"] if elf else None,
            "bin_sha256": sha256_file(bin_path) if bin_path and os.path.isfile(bin_path) else None,
            "commit": _first_line(os.path.join(outdir, "base.id" if a.name == "pio_base" else "s1.id")),
            "pio_version": _pio_version(),
            "platform_espressif32": _platform_version(),
        }

    append_record(manifest, rec)
    if missing and a.exit == 0:
        print(f"check_evidence_manifest: step '{a.name}' exited 0 but did not produce: "
              + ", ".join(missing), file=sys.stderr)
        return EXIT_MISSING_OUTPUT
    return 0


# --------------------------------------------------------------------------- --consume

def _producers(recs: list[dict], upto: int | None = None) -> dict[str, tuple[str, str, str]]:
    """Map normalized path -> (sha256, kind, producer) from exit-0 step records.

    Later successful producers win. Failed steps never count as producers: an output
    of a failed build is exactly the kind of half-written artefact the rule excludes.
    """
    out: dict[str, tuple[str, str, str]] = {}
    for rec in recs[:upto]:
        if rec.get("type") != "step" or rec.get("exit") != 0:
            continue
        for o in rec.get("outputs") or []:
            if isinstance(o, dict) and "path" in o and "sha256" in o:
                out[o["path"]] = (o["sha256"], o.get("kind", "file"), str(rec.get("name")))
    return out


def _check_inputs(recs: list[dict], outdir: str, paths: list[str],
                  upto: int | None = None) -> tuple[list[dict], list[str]]:
    prod = _producers(recs, upto)
    inputs: list[dict] = []
    errors: list[str] = []
    for p in paths:
        np_ = norm(p)
        if not inside(p, outdir):
            errors.append(f"{p}: outside this invocation's $OUT ({outdir})")
            continue
        if np_ not in prod:
            errors.append(f"{p}: not an output of any earlier successful step in the manifest")
            continue
        want, _kind, producer = prod[np_]
        try:
            _k, have = hash_path(p)
        except FileNotFoundError:
            errors.append(f"{p}: recorded by step '{producer}' but no longer exists")
            continue
        if have != want:
            errors.append(f"{p}: sha256 {have} differs from {want} recorded by step '{producer}' "
                          "(modified after it was produced)")
            continue
        inputs.append({"path": np_, "sha256": have, "producer": producer})
    return inputs, errors


def cmd_consume(manifest: str, paths: list[str], cmd: list[str]) -> int:
    outdir = os.path.dirname(os.path.abspath(manifest))
    if not cmd:
        print("check_evidence_manifest: --consume needs `-- cmd args...`", file=sys.stderr)
        return EXIT_CONSUME_REFUSED
    if not paths:
        print("check_evidence_manifest: --consume needs at least one input path", file=sys.stderr)
        return EXIT_CONSUME_REFUSED
    try:
        recs = load_manifest(manifest)
    except (OSError, ValueError) as e:
        print(f"check_evidence_manifest: REFUSED: cannot read manifest: {e}", file=sys.stderr)
        return EXIT_CONSUME_REFUSED
    inputs, errors = _check_inputs(recs, outdir, paths)
    if errors:
        for e in errors:
            print(f"check_evidence_manifest: REFUSED: {e}", file=sys.stderr)
        print("check_evidence_manifest: comparison NOT run: " + " ".join(cmd), file=sys.stderr)
        return EXIT_CONSUME_REFUSED
    append_record(manifest, {"type": "consume", "inputs": inputs, "cmd": cmd})
    sys.stdout.flush()
    sys.stderr.flush()
    try:
        return subprocess.run(cmd).returncode
    except OSError as e:
        print(f"check_evidence_manifest: cannot run {cmd[0]}: {e}", file=sys.stderr)
        return 127


# --------------------------------------------------------------------------- --verify

# A placeholder is `<` + non-space ... non-space + `>` on one line: catches `<BASE EVQ_OUT>`
# and `<…>` (the frozen block once shipped with those) while NOT matching shell
# redirections like `sort <in >out` (whitespace right before `>`). Heredoc `<<` / `<<-`
# is blanked first.
_PLACEHOLDER = re.compile(r"<[^<>\s](?:[^<>\n]*[^<>\s])?>")
_HEREDOC = re.compile(r"<<-?")
# `$NAME` or `${NAME...}` / `${#NAME}` / `${!NAME}`. Positional and special parameters
# ($1..$9, $@, $*, $#, $?, $$, $!, $-, $0, ${1:-x}) never match because the name must
# start with a letter or underscore. `$(`/`\$(` (command substitution) and `$((` never
# match either. An escaped `\$NAME` is a reference in an inner `sh -c` shell and still
# counts: that shell only sees exported environment, so it must be assigned here too.
_VARREF = re.compile(r"\$(?:\{[#!]?([A-Za-z_][A-Za-z0-9_]*)|([A-Za-z_][A-Za-z0-9_]*))")
_ASSIGN = re.compile(r"(?:^|[\s;&|(`{])([A-Za-z_][A-Za-z0-9_]*)\+?=")
_DECL = re.compile(r"\b(?:local|declare|typeset|readonly|export)((?:\s+-[A-Za-z]+)*)"
                   r"((?:\s+[A-Za-z_][A-Za-z0-9_]*(?:=[^\s;&|]*)?)+)")
_FOR = re.compile(r"\bfor\s+([A-Za-z_][A-Za-z0-9_]*)\s+in\b")
_READ = re.compile(r"\bread((?:\s+-[A-Za-z]+)*)((?:\s+[A-Za-z_][A-Za-z0-9_]*)+)")
_STEPNAME = re.compile(r"^\s*step\s+([A-Za-z0-9_.\-]+)(?:\s|$)")


def script_assigned(text: str) -> set[str]:
    names = set(_ASSIGN.findall(text)) | set(_FOR.findall(text))
    for m in _DECL.finditer(text):
        for tok in m.group(2).split():
            names.add(tok.split("=", 1)[0])
    for m in _READ.finditer(text):
        names.update(m.group(2).split())
    return names


def static_check(text: str) -> list[str]:
    """Return a list of problems with the evidence script (empty = PASS)."""
    problems: list[str] = []
    for lineno, line in enumerate(text.splitlines(), 1):
        for m in _PLACEHOLDER.finditer(_HEREDOC.sub("  ", line)):
            problems.append(f"line {lineno}: angle-bracket placeholder {m.group(0)!r}")
    assigned = script_assigned(text)
    seen: set[str] = set()
    for lineno, line in enumerate(text.splitlines(), 1):
        for m in _VARREF.finditer(line):
            name = m.group(1) or m.group(2)
            if name not in assigned and name not in seen:
                seen.add(name)
                problems.append(f"line {lineno}: ${name} is referenced but never assigned in the script")
    return problems


def script_steps(text: str) -> list[str]:
    out: list[str] = []
    for line in text.splitlines():
        m = _STEPNAME.match(line)
        if m and m.group(1) not in out:
            out.append(m.group(1))
    return out


def cmd_verify(script: str, manifest: str, out: str) -> int:
    ok = True

    def res(passed: bool, what: str) -> None:
        nonlocal ok
        ok = ok and passed
        print(f"{'PASS' if passed else 'FAIL'} {what}")

    # (i) static script check
    try:
        text = Path(script).read_text(encoding="utf-8")
    except OSError as e:
        res(False, f"script readable: {e}")
        return 1
    problems = static_check(text)
    res(not problems, "script has no placeholders / unassigned variables")
    for p in problems:
        print(f"     {p}")

    # (ii) manifest
    try:
        recs = load_manifest(manifest)
    except (OSError, ValueError) as e:
        res(False, f"manifest readable: {e}")
        return 1
    res(bool(recs), "manifest is non-empty")
    if not recs:
        return 1

    outdir = os.path.dirname(os.path.abspath(manifest))
    res(os.path.realpath(outdir) == os.path.realpath(out),
        f"manifest lives in --out ({manifest} vs {out})")

    first = recs[0]
    init_ok = (first.get("type") == "step" and first.get("name") == "init" and first.get("exit") == 0
               and any(isinstance(o, dict) and o.get("path") == norm(out) and o.get("kind") == "dir"
                       and o.get("sha256") == EMPTY_SHA256 for o in first.get("outputs") or []))
    res(init_ok, "first record is a successful `init` step that saw --out as an EMPTY directory "
                 "(fresh, never reused)")

    steps = [r for r in recs if r.get("type") == "step"]
    for name in script_steps(text):
        mine = [r for r in steps if r.get("name") == name]
        good = len(mine) == 1 and mine[0].get("exit") == 0 and not mine[0].get("missing")
        detail = ("missing" if not mine else f"{len(mine)} record(s), exit={[r.get('exit') for r in mine]}"
                  + (f", missing outputs={mine[0].get('missing')}" if mine and mine[0].get("missing") else ""))
        res(good, f"step {name}: exactly one successful record" + ("" if good else f" ({detail})"))

    for i, rec in enumerate(recs):
        if rec.get("type") != "consume":
            continue
        paths = [inp.get("path", "") for inp in rec.get("inputs") or [] if isinstance(inp, dict)]
        inputs, errors = _check_inputs(recs, out, paths, upto=i)
        # The consume-time hash must also be the producer's hash (belt and braces: a
        # hand-edited consume record cannot claim a different input).
        byp = {inp["path"]: inp["sha256"] for inp in inputs}
        for inp in rec.get("inputs") or []:
            if isinstance(inp, dict) and inp.get("path") in byp and inp.get("sha256") != byp[inp["path"]]:
                errors.append(f"{inp['path']}: consume record hash differs from producer's")
        if not paths:
            errors.append("consume record has no inputs")
        cmdtxt = " ".join(str(c) for c in rec.get("cmd") or [])[:120]
        res(not errors, f"consume #{i} ({cmdtxt}): inputs inside $OUT, produced earlier, unchanged")
        for e in errors:
            print(f"     {e}")

    print("VERIFY " + ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


# --------------------------------------------------------------------------- --compare-runs

def cmd_compare_runs(a_dir: str, b_dir: str) -> int:
    """Informational only (§5.1): builds are NOT assumed reproducible, so equal or
    different ELF hashes across invocations say nothing about freshness. Always 0."""
    fw: list[dict[str, dict]] = []
    for d in (a_dir, b_dir):
        m = os.path.join(d, "manifest.jsonl")
        try:
            recs = load_manifest(m)
        except (OSError, ValueError) as e:
            print(f"INFO cannot read {m}: {e}")
            recs = []
        fw.append({r["name"]: r["firmware"] for r in recs
                   if r.get("type") == "step" and isinstance(r.get("firmware"), dict) and "name" in r})
    common = [n for n in fw[0] if n in fw[1]]
    if not common:
        print("INFO no firmware steps present in both runs")
    for n in common:
        ea, eb = fw[0][n].get("elf_sha256"), fw[1][n].get("elf_sha256")
        verdict = "equal" if ea == eb and ea is not None else "different"
        print(f"INFO {n}: elf_sha256 {verdict} (A={ea} B={eb}; "
              f"commit A={fw[0][n].get('commit')} B={fw[1][n].get('commit')})")
    print("INFO cross-run comparison is informational; it never gates the evidence")
    return 0


# --------------------------------------------------------------------------- cli

def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="check_evidence_manifest.py",
        description="Evidence-provenance manifest: record step outputs, gate comparisons on "
                    "fresh inputs, verify a whole invocation (contract §5.1, C-00b).",
        epilog="Usage forms:\n"
               "  --record --manifest M --name N --start T0 --end T1 --exit RC --outputs 'p1,p2'\n"
               "  --consume M path... -- cmd args...\n"
               "  --verify --script S --manifest M --out O\n"
               "  --compare-runs A B      (informational, always exits 0)\n"
               "Exit codes: 3 = zero-exit step missing an output; 4 = consume refused "
               "(command not run).",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        allow_abbrev=False)
    g = p.add_mutually_exclusive_group(required=True)
    g.add_argument("--record", action="store_true", help="append one step record")
    g.add_argument("--consume", metavar="MANIFEST",
                   help="check the paths that follow, then run the command after `--`")
    g.add_argument("--verify", action="store_true", help="verify script + manifest")
    g.add_argument("--compare-runs", nargs=2, metavar=("A", "B"),
                   help="compare firmware ELF hashes of two run dirs (informational)")
    p.add_argument("--manifest")
    p.add_argument("--name")
    p.add_argument("--start")
    p.add_argument("--end")
    p.add_argument("--exit", type=int)
    p.add_argument("--outputs", help="comma-separated output paths ('' = none)")
    p.add_argument("--script")
    p.add_argument("--out")
    p.add_argument("paths", nargs="*", help="(--consume) input paths")
    return p


def main(argv: list[str] | None = None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    cmd: list[str] = []
    if "--" in argv:
        i = argv.index("--")
        argv, cmd = argv[:i], argv[i + 1:]
    p = build_parser()
    a = p.parse_args(argv)

    if a.record:
        need = {"--manifest": a.manifest, "--name": a.name, "--start": a.start, "--end": a.end,
                "--exit": a.exit, "--outputs": a.outputs}
        absent = [k for k, v in need.items() if v is None]
        if absent:
            p.error("--record requires " + ", ".join(absent))
        if a.paths or cmd:
            p.error("--record takes no positional arguments")
        return cmd_record(a)
    if a.consume is not None:
        if any(v is not None for v in (a.manifest, a.name, a.start, a.end, a.exit, a.outputs,
                                       a.script, a.out)):
            p.error("--consume takes only: MANIFEST path... -- cmd args...")
        if not cmd:
            p.error("--consume requires `-- cmd args...`")
        return cmd_consume(a.consume, a.paths, cmd)
    if a.verify:
        absent = [k for k, v in {"--script": a.script, "--manifest": a.manifest, "--out": a.out}.items()
                  if v is None]
        if absent:
            p.error("--verify requires " + ", ".join(absent))
        if a.paths or cmd:
            p.error("--verify takes no positional arguments")
        return cmd_verify(a.script, a.manifest, a.out)
    if a.paths or cmd:
        p.error("--compare-runs takes exactly two run directories")
    return cmd_compare_runs(*a.compare_runs)


if __name__ == "__main__":
    sys.exit(main())
