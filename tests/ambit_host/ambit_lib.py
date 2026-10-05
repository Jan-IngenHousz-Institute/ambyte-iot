"""AMBIT SD writers under host test (tests/ambit_host).

HEAD compiles the production components/ambit_ota/ambit_stage.c and
components/ambit_flash/ambit_flash_preflight.c through the media shim. The
ambit_flash_image prefix (everything up to taking the UART bus) and, for the
baseline, http_get_to_file_impl are SLICED VERBATIM out of `git show <rev>:`
sources; the only rewrite is the "/sdcard" -> "./sdcard" path literal so the
code runs inside a scratch directory. Every slice is written next to the build
for inspection."""
from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HOST = ROOT / "tests" / "ambit_host"
SHIM = ROOT / "tests" / "sd_shim"
SAN = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
REGIONS = ["bootloader.bin", "partitions.bin", "boot_app0.bin", "app.bin"]


def cc() -> str:
    c = os.environ.get("CC") or shutil.which("clang") or "clang"
    if os.path.basename(c) == "cc":
        c = shutil.which("clang") or "clang"
    return c


def _run(cmd: list[str]) -> None:
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"compile failed: {' '.join(cmd)}\n{r.stdout}\n{r.stderr}")


def _src(rev: str | None, rel: str) -> str:
    if rev is None:
        return (ROOT / rel).read_text()
    return subprocess.run(["git", "show", f"{rev}:{rel}"], cwd=ROOT, capture_output=True, text=True,
                          check=True).stdout


def _func(text: str, signature_start: str) -> str:
    """Slice one C function definition (signature through its matching brace)."""
    i = text.index(signature_start)
    j = text.index("{", i)
    depth = 0
    for k in range(j, len(text)):
        if text[k] == "{":
            depth += 1
        elif text[k] == "}":
            depth -= 1
            if depth == 0:
                return text[i:k + 1]
    raise ValueError("unbalanced function " + signature_start)


def _rewrite(s: str) -> str:
    return s.replace('"/sdcard', '"./sdcard')


def slice_flash(rev: str | None) -> str:
    """ambit_flash_image up to (and including) the UART-bus call, verbatim."""
    t = _src(rev, "components/ambit_flash/ambit_flash.c")
    regions = t[t.index("typedef struct {\n    uint32_t    offset;"):t.index("#define NUM_REGIONS")]
    num = t[t.index("#define NUM_REGIONS"):].split("\n", 1)[0]
    root = t[t.index("#define AMBIT_FW_ROOT"):].split("\n", 1)[0]
    fn_start = t.index("esp_err_t ambit_flash_image(uint8_t channel, const char *dir, uint32_t baud,")
    cut = t.index("uart_sensors_flash_session_begin(channel, BUS_WAIT_MS);", fn_start)
    cut = t.index("\n", cut) + 1
    fn = t[fn_start:cut]
    helper = ""
    if "static long region_file_size(" in t:
        helper = _func(t, "static long region_file_size(")
    body = f"""/* GENERATED slice of components/ambit_flash/ambit_flash.c @ {rev or 'working tree'} */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_err.h"
#include "esp_log.h"
#include "sd_card.h"
{'#include "ambit_flash_preflight.h"' if rev is None else ''}
#define TAG "ambit_flash"
#define BUS_WAIT_MS 3000
#define FLASH_BAUD_DEF 460800
#define UART_SENSOR_NUM_CHANNELS 4
typedef struct {{ int chip; unsigned regions; unsigned bytes; }} ambit_flash_image_result_t;
esp_err_t uart_sensors_flash_session_begin(uint8_t ch, uint32_t wait_ms);
{_rewrite(root)}
{regions}{num}
{helper}
{fn}    return e;
}}

esp_err_t slice_flash_image(uint8_t channel, const char *dir)
{{
    ambit_flash_image_result_t r;
    return ambit_flash_image(channel, dir, 0, &r);
}}
"""
    return body


def slice_ota_base(rev: str) -> str:
    t = _src(rev, "components/ambit_ota/ambit_ota.c")
    fn = _func(t, "static esp_err_t http_get_to_file_impl(const char *url, const char *path, size_t *out_size)\n{")
    return f"""/* GENERATED slice of components/ambit_ota/ambit_ota.c @ {rev} (http_get_to_file_impl, verbatim) */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_err.h"
#include "esp_log.h"
#include "http_stub.h"
#define TAG "ambit_ota"
#define AMBIT_OTA_DL_BUF 4096
{fn}

esp_err_t base_http_get_to_file(const char *url, const char *path, size_t *out_size)
{{
    return http_get_to_file_impl(url, path, out_size);
}}
"""


def build(out_dir: Path, rev: str | None) -> Path:
    out_dir.mkdir(parents=True, exist_ok=True)
    name = "ambit_head" if rev is None else f"ambit_base_{rev}"
    gen = out_dir / (name + ".gen")
    gen.mkdir(exist_ok=True)
    (gen / "slice_flash.c").write_text(slice_flash(rev))
    incs = [f"-I{SHIM / 'stubs'}", f"-I{SHIM}", f"-I{HOST}", f"-I{ROOT / 'components/sd_card'}",
            f"-I{ROOT / 'components/event_log/include'}", f"-I{ROOT / 'components/ambit_ota'}",
            f"-I{ROOT / 'components/ambit_flash'}"]
    defs = ['-DSD_MOUNT_POINT="./sdcard"'] + (["-DAMBIT_BASE"] if rev is not None else [])
    warn = ["-Wall", "-Wextra"] + (["-Werror"] if rev is None else [])
    base = [cc(), "-std=gnu11", "-g", "-O1", *SAN, *warn, *incs, *defs]
    objdir = out_dir / (name + ".o")
    objdir.mkdir(exist_ok=True)
    shimmed = [gen / "slice_flash.c"]
    if rev is None:
        shimmed += [ROOT / "components/ambit_ota/ambit_stage.c", ROOT / "components/ambit_flash/ambit_flash_preflight.c"]
    else:
        (gen / "slice_ota.c").write_text(slice_ota_base(rev))
        shimmed.append(gen / "slice_ota.c")
    plain = [SHIM / "sd_shim.c", SHIM / "sd_host_stubs.c", SHIM / "host_rtos.c", SHIM / "mbedtls_sha256_host.c",
             ROOT / "components/sd_card/sd_diag_core.c", HOST / "http_stub.c", HOST / "ambit_driver.c"]
    objs = []
    for s in shimmed:
        o = objdir / (s.stem + ".o")
        _run([*base, "-Wno-error=unused-parameter", "-include", str(SHIM / "sd_shim.h"), "-c", str(s), "-o", str(o)])
        objs.append(o)
    for s in plain:
        o = objdir / (s.stem + ".o")
        _run([*base, "-Wno-error", "-c", str(s), "-o", str(o)])
        objs.append(o)
    exe = out_dir / name
    _run([cc(), *SAN, *map(str, objs), "-lpthread", "-o", str(exe)])
    return exe


@dataclass
class Dev:
    exe: Path
    state: Path
    proc: subprocess.Popen | None = None
    exits: list = field(default_factory=list)

    def start(self) -> None:
        env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0")
        self.proc = subprocess.Popen([str(self.exe), str(self.state)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     text=True, env=env)

    def cmd(self, line: str) -> dict | None:
        assert self.proc and self.proc.stdin and self.proc.stdout
        self.proc.stdin.write(line + "\n")
        self.proc.stdin.flush()
        if line.split()[0] in ("teardown_at", "teardown_now"):
            return None
        out = self.proc.stdout.readline()
        if not out:                                  # the process died (simulated CPU reset)
            rc = self.proc.wait(timeout=30)
            self.exits.append(rc)
            self.proc = None
            return {"crashed": rc}
        return json.loads(out)

    def reboot(self) -> None:
        if self.proc:
            self.proc.stdin.write("quit\n")
            self.proc.stdin.flush()
            self.proc.wait(timeout=30)
        self.start()

    def finish(self) -> None:
        if self.proc:
            self.proc.stdin.write("quit\n")
            self.proc.stdin.flush()
            self.proc.wait(timeout=30)
            self.proc = None

    def ops(self) -> list[dict]:
        p = self.state / "ops.jsonl"
        return [json.loads(x) for x in p.read_text().splitlines() if x.strip()] if p.exists() else []


def new_dev(exe: Path, state: Path) -> Dev:
    if state.exists():
        shutil.rmtree(state)
    (state / "sdcard").mkdir(parents=True)
    return Dev(exe=exe, state=state)
