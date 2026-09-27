#!/usr/bin/env bash
set -euo pipefail
ROLE_DIR=/home/dv/.local/share/ambyte-write-integrity/sprint-01/${1:-generator}
REPO=/home/dv/.traycer/worktrees/jan-ingenhousz-institute__ambyte-iot/fix-sd-write-integrity
BASE_DIR=/home/dv/.local/share/ambyte-write-integrity/sprint-01/base-e1ca6ee
BASE_SHA=e1ca6ee9bb1cecb497014d810966bd4dc4410279
cd "$REPO"
mkdir -p -m 700 "$ROLE_DIR"
OUT="$ROLE_DIR/run-$(date -u +%Y%m%dT%H%M%SZ)-$$"
mkdir -m 700 "$OUT"                                   # fails (aborts) if it already exists: never reused
MANIFEST="$OUT/manifest.jsonl"
step() { local n=$1 outs=$2; shift 2; local t0 rc; t0=$(date +%s.%N)
  if "$@"; then rc=0; else rc=$?; fi
  python tools/check_evidence_manifest.py --record --manifest "$MANIFEST" --name "$n" --start "$t0" \
         --end "$(date +%s.%N)" --exit "$rc" --outputs "$outs"
  return "$rc"; }
step init "$OUT" true

# 0. identities: S1 commit + verified detached baseline (tracked-file changes allowed only in the generated
#    sdkconfig.esp32-s3-devkitm-1, whose diff is recorded as evidence; anything else aborts)
step s1_id   "$OUT/s1.id"   sh -c "git rev-parse HEAD > '$OUT/s1.id' && git status --porcelain --untracked-files=no > '$OUT/s1.dirty' \
                                   && ! grep -v ' sdkconfig.esp32-s3-devkitm-1$' '$OUT/s1.dirty' && git diff -- sdkconfig.esp32-s3-devkitm-1 >> '$OUT/s1.id'"
test -d "$BASE_DIR" || git worktree add --detach "$BASE_DIR" "$BASE_SHA"
git -C "$BASE_DIR" submodule update --init --recursive components/littlefs
step base_id "$OUT/base.id" sh -c "test \"\$(git -C '$BASE_DIR' rev-parse HEAD)\" = '$BASE_SHA' && git -C '$BASE_DIR' rev-parse HEAD > '$OUT/base.id' \
                                   && git -C '$BASE_DIR' status --porcelain --untracked-files=no > '$OUT/base.dirty' \
                                   && ! grep -v ' sdkconfig.esp32-s3-devkitm-1$' '$OUT/base.dirty' && git -C '$BASE_DIR' diff -- sdkconfig.esp32-s3-devkitm-1 >> '$OUT/base.id'"

# 1. RED/GREEN (base sources via git show $BASE_SHA + working tree; exit 0 only if every specified base signature is RED and HEAD GREEN)
step sdlog_rg "$OUT/rg_sdlog" python tests/sdlog_host/red_green.py --rev "$BASE_SHA" --out "$OUT/rg_sdlog"
step evq_rg   "$OUT/rg_evq"   python tests/evq_host/red_green.py   --rev "$BASE_SHA" --out "$OUT/rg_evq"
step ambit_rg "$OUT/rg_ambit" python tests/ambit_host/red_green.py --rev "$BASE_SHA" --out "$OUT/rg_ambit"

# 2. evq regression: baseline groups (baseline runner), S1 groups + X, matrix, determinism, dup comparison
step evq_base "$OUT/evq_base" env -C "$BASE_DIR" EVQ_OUT="$OUT/evq_base" EVQ_TMPDIR="$OUT/tmp_base" \
                              python tests/evq_host/run.py --groups A,B,C,E,F,G,H,Q,HIL --seeds 1,7,1337
step evq_s1   "$OUT/evq_s1"   env EVQ_OUT="$OUT/evq_s1" EVQ_TMPDIR="$OUT/tmp_s1" \
                              python tests/evq_host/run.py --groups A,B,C,E,F,G,H,Q,HIL,X --seeds 1,7,1337
step evq_mx   "$OUT/evq_mx"   env EVQ_OUT="$OUT/evq_mx" EVQ_TMPDIR="$OUT/tmp_mx" \
                              python tests/evq_host/run.py --matrix --seeds 1,7,1337 --coverage-report
step det1     "$OUT/det1"     env EVQ_OUT="$OUT/det1" EVQ_TMPDIR="$OUT/tmp_d1" python tests/evq_host/run.py --groups X --seeds 1,7
step det2     "$OUT/det2"     env EVQ_OUT="$OUT/det2" EVQ_TMPDIR="$OUT/tmp_d2" python tests/evq_host/run.py --groups X --seeds 1,7
step det_cmp  ""              python tools/check_evidence_manifest.py --consume "$MANIFEST" "$OUT/det1" "$OUT/det2" -- \
                              python tests/evq_host/compare_evidence.py "$OUT/det1" "$OUT/det2"
step dup_cmp  ""              python tools/check_evidence_manifest.py --consume "$MANIFEST" "$OUT/evq_base" "$OUT/evq_s1" -- \
                              python tests/evq_host/compare_dups.py "$OUT/evq_base" "$OUT/evq_s1"

# 3. pytest (CC=clang; clang baseline 287 passed / 0 failed; CC=gcc gives 106 Fedora-GCC -Werror environment errors)
step py_base_c "$OUT/base-collect.txt" sh -c "cd '$BASE_DIR' && CC=clang python -m pytest tests -q --collect-only -p no:cacheprovider > '$OUT/base-collect.txt'"
step py_base   "$OUT/base.xml"         sh -c "cd '$BASE_DIR' && CC=clang python -m pytest tests -q -p no:cacheprovider --junitxml='$OUT/base.xml'"
step py_s1_c   "$OUT/s1-collect.txt"   sh -c "CC=clang python -m pytest tests -q --collect-only -p no:cacheprovider > '$OUT/s1-collect.txt'"
step py_s1     "$OUT/s1.xml"           sh -c "CC=clang python -m pytest tests -q -p no:cacheprovider --junitxml='$OUT/s1.xml'"
step py_cmp    "" python tools/check_evidence_manifest.py --consume "$MANIFEST" "$OUT/base-collect.txt" "$OUT/s1-collect.txt" "$OUT/base.xml" "$OUT/s1.xml" -- \
                  python tests/evq_host/compare_suites.py "$OUT/base-collect.txt" "$OUT/s1-collect.txt" "$OUT/base.xml" "$OUT/s1.xml"

# 4. fresh builds into this invocation's build dirs, budget, release exclusion
REL="$OUT/build_rel/esp32-s3-devkitm-1"; HIL="$OUT/build_hil/evq-hil"; BAS="$OUT/build_base/esp32-s3-devkitm-1"
step pio_rel  "$REL/firmware.elf,$REL/firmware.bin,$REL/config/sdkconfig.json" env PLATFORMIO_BUILD_DIR="$OUT/build_rel" pio run -e esp32-s3-devkitm-1
step pio_hil  "$HIL/firmware.elf,$HIL/firmware.bin,$HIL" env PLATFORMIO_BUILD_DIR="$OUT/build_hil" pio run -e evq-hil
step pio_base "$BAS/firmware.elf,$BAS/firmware.bin" env PLATFORMIO_BUILD_DIR="$OUT/build_base" pio run -d "$BASE_DIR" -e esp32-s3-devkitm-1
step budget   "" python tools/check_evidence_manifest.py --consume "$MANIFEST" "$BAS/firmware.elf" "$REL/firmware.elf" -- \
                 python tools/elf_budget.py --base "$BAS/firmware.elf" --head "$REL/firmware.elf" --max-flash 8192 --max-dram 1024 --max-rtc-noinit 256
step nohil    "" python tools/check_evidence_manifest.py --consume "$MANIFEST" "$REL/firmware.elf" "$REL/firmware.bin" "$REL/config/sdkconfig.json" -- \
                 python tools/check_release_no_hil.py --elf "$REL/firmware.elf" --bin "$REL/firmware.bin" --sdkconfig-json "$REL/config/sdkconfig.json"
step nohil_st "" python tools/check_release_no_hil.py --self-test
step hil_wrap "" python tools/check_evidence_manifest.py --consume "$MANIFEST" "$HIL/firmware.elf" "$HIL" -- \
                 python tools/check_release_no_hil.py --expect-wrapped --elf "$HIL/firmware.elf" --objdir "$HIL"   # C-36

# 5. manifest self-check over the whole invocation
python tools/check_evidence_manifest.py --verify --script tools/sprint1_evidence.sh --manifest "$MANIFEST" --out "$OUT"
