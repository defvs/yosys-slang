#!/usr/bin/env bash
# Copyright (c) 2026 The yosys-slang contributors
# SPDX-License-Identifier: ISC

STRESS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ROOT="$(cd "$STRESS_DIR/../.." && pwd)"
OT="$ROOT/tests/third_party/opentitan"
BUILD_ROOT="${BUILD_ROOT:-$STRESS_DIR/build}"
PLUGIN="${PLUGIN:-$ROOT/build/slang.so}"
YOSYS="${YOSYS:-yosys}"
SMTBMC="${SMTBMC:-yosys-smtbmc}"
SOLVER="${SOLVER:-yices}"
FILTERED="$BUILD_ROOT/filtered"

prepare_source() {
  local src="$1"
  local dst="$2"
  python3 "$STRESS_DIR/common/prepare_sources.py" --src "$src" --dst "$dst"
}

run_case() {
  local name="$1"
  local top="$2"
  local depth="$3"
  shift 3

  local case_dir="$BUILD_ROOT/$name"
  local ys="$case_dir/$name.ys"
  local smt="$case_dir/$name.smt2"
  local full_il="$case_dir/$name.full.il"
  local synth_log="$case_dir/synth.log"
  local bmc_log="$case_dir/bmc.log"
  mkdir -p "$case_dir"

  {
    cat <<EOF
read_slang --no-synthesis-define --ignore-timing --single-unit -D FPV_ON --top $top \\
  -I$OT/hw/ip/prim/rtl \\
  -I$OT/hw/ip/prim_generic/rtl \\
  -I$OT/hw/ip/prim/fpv/vip \\
  -I$FILTERED \\
EOF
    local files=("$@")
    local idx
    for idx in "${!files[@]}"; do
      if [ "$idx" -eq $((${#files[@]} - 1)) ]; then
        printf '  %s\n' "${files[$idx]}"
      else
        printf '  %s \\\n' "${files[$idx]}"
      fi
    done
    cat <<EOF
prep -top $top
async2sync
dffunmap
chformal -lower
write_rtlil $full_il
chformal -live -remove
chformal -fair -remove
write_smt2 -wires $smt
EOF
  } > "$ys"

  local synth_status=PASS
  if ! "$YOSYS" -m "$PLUGIN" "$ys" >"$synth_log" 2>&1; then
    synth_status=FAIL
  fi
  if [ "$synth_status" = PASS ] && [ "${EXPECT_LIVENESS:-0}" = 1 ]; then
    if ! grep -Eq 'cell \$(live|fair) ' "$full_il"; then
      echo "Expected liveness/fairness cells were not generated" >>"$synth_log"
      synth_status=FAIL
    fi
  fi

  local bmc_status=SKIP
  if [ "$synth_status" = PASS ]; then
    if "$SMTBMC" --keep-going -s "$SOLVER" -t "$depth" "$smt" >"$bmc_log" 2>&1; then
      bmc_status=PASS
    else
      bmc_status=FAIL
    fi
  fi

  {
    echo "$name|$synth_status|$bmc_status|$case_dir"
    if [ "$synth_status" = FAIL ]; then
      grep -E 'ERROR:|Assert failed|Unsupported|internal error|Exception' "$synth_log" | head -20 || true
    elif [ "$bmc_status" = FAIL ]; then
      grep -E 'Assert failed|Status:|BMC failed|FAILED' "$bmc_log" || true
    else
      grep -E 'Status:|PASSED' "$bmc_log" || true
    fi
  } > "$case_dir/result.txt"

  [ "$synth_status" = PASS ] && [ "$bmc_status" = PASS ]
}
