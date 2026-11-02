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

"$STRESS_DIR/common/fetch_opentitan.sh" "$OT"

prepare_source() {
  local src="$1"
  local dst="$2"
  python3 "$STRESS_DIR/common/prepare_sources.py" --src "$src" --dst "$dst"
}

# Count the checks in a model written by write_smt2
count_checks() {
  grep -c -E '^; yosys-smt2-(assert|assume|cover) ' "$1" || true
}

# Synthesize a harness and run BMC on it. A property dropped in lowering would
# pass BMC unnoticed, so the case also fails unless the model has the expected
# number of checks.
run_case() {
  local name="$1"
  local top="$2"
  local depth="$3"
  local expected_checks="$4"
  shift 4

  local case_dir="$BUILD_ROOT/$name"
  local ys="$case_dir/$name.ys"
  local smt="$case_dir/$name.smt2"
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
write_smt2 -wires $smt
EOF
  } > "$ys"

  local synth_status=PASS
  if ! "$YOSYS" -m "$PLUGIN" "$ys" >"$synth_log" 2>&1; then
    synth_status=FAIL
  fi

  local checks_status=SKIP
  local checks=
  if [ "$synth_status" = PASS ]; then
    checks=$(count_checks "$smt")
    if [ "$checks" = "$expected_checks" ]; then
      checks_status=PASS
    else
      checks_status=FAIL
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
    echo "$name|$synth_status|$checks_status|$bmc_status|$case_dir"
    if [ "$synth_status" = FAIL ]; then
      grep -E 'ERROR:|Assert failed|Unsupported|internal error|Exception' "$synth_log" | head -20 || true
    fi
    if [ "$checks_status" = FAIL ]; then
      echo "Expected $expected_checks checks, found $checks."
    fi
    if [ "$bmc_status" = FAIL ]; then
      grep -E 'Assert failed|Status:|BMC failed|FAILED' "$bmc_log" || true
    elif [ "$bmc_status" = PASS ]; then
      grep -E 'Status:|PASSED' "$bmc_log" || true
    fi
  } > "$case_dir/result.txt"

  [ "$synth_status" = PASS ] && [ "$checks_status" = PASS ] && [ "$bmc_status" = PASS ]
}
