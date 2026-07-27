#!/usr/bin/env bash
# Copyright (c) 2026 The yosys-slang contributors
# SPDX-License-Identifier: ISC

set -euo pipefail

CASE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEPTH="${DEPTH:-8}"
source "$CASE_DIR/../common/common.sh"

BUILD="$BUILD_ROOT/tlul"
ASSUME_LEGAL_D2H="${ASSUME_LEGAL_D2H:-1}"
ASSUME_LEGAL_D2H_IMMEDIATE="${ASSUME_LEGAL_D2H_IMMEDIATE:-0}"
EXPLICIT_TLUL_CLOCKING="${EXPLICIT_TLUL_CLOCKING:-0}"
SLANG_DEFINES=()
if [ "$ASSUME_LEGAL_D2H" = 1 ]; then
  SLANG_DEFINES=(-D TLUL_ASSUME_LEGAL_D2H)
fi
if [ "$ASSUME_LEGAL_D2H_IMMEDIATE" = 1 ]; then
  SLANG_DEFINES=(-D TLUL_ASSUME_LEGAL_D2H_IMMEDIATE)
fi

TLUL_ASSERT_SRC="$OT/hw/ip/tlul/rtl/tlul_assert.sv"
TLUL_ASSERT_FILTERED="$BUILD/tlul_assert.filtered.sv"
SYNTH_LOG="$BUILD/synth.log"
SMT="$BUILD/tlul_sva.smt2"
FULL_IL="$BUILD/tlul_sva.full.il"
BMC_LOG="$BUILD/bmc.log"
SUMMARY="$BUILD/summary.md"
RESULT="$BUILD/result.txt"

mkdir -p "$BUILD"

PREPARE_ARGS=(
  --src "$TLUL_ASSERT_SRC"
  --dst "$TLUL_ASSERT_FILTERED"
)
if [ "$EXPLICIT_TLUL_CLOCKING" = 1 ]; then
  PREPARE_ARGS+=(--explicit-clocking)
fi

python3 "$CASE_DIR/prepare_tlul_assert.py" \
  "${PREPARE_ARGS[@]}"

cat > "$BUILD/tlul_sva.ys" <<EOF
read_slang --no-synthesis-define --ignore-timing --single-unit --top tlul_sva_harness \\
  ${SLANG_DEFINES[*]} \\
  -I$OT/hw/ip/prim/rtl \\
  $OT/hw/top_earlgrey/rtl/top_pkg.sv \\
  $OT/hw/ip/prim/rtl/prim_mubi_pkg.sv \\
  $OT/hw/ip/prim/rtl/prim_secded_pkg.sv \\
  $OT/hw/ip/tlul/rtl/tlul_pkg.sv \\
  $TLUL_ASSERT_FILTERED \\
  $CASE_DIR/harness.sv
prep -top tlul_sva_harness
async2sync
dffunmap
chformal -lower
write_rtlil $FULL_IL
chformal -live -remove
chformal -fair -remove
write_smt2 -wires $SMT
EOF

synth_status=PASS
if ! "$YOSYS" -m "$PLUGIN" "$BUILD/tlul_sva.ys" >"$SYNTH_LOG" 2>&1; then
  synth_status=FAIL
fi
if [ "$synth_status" = PASS ] && ! grep -Eq 'cell \$(live|fair) ' "$FULL_IL"; then
  echo "Expected liveness/fairness cells were not generated" >>"$SYNTH_LOG"
  synth_status=FAIL
fi

bmc_status=SKIP
if [ "$synth_status" = PASS ]; then
  if "$SMTBMC" --keep-going -s "$SOLVER" -t "$DEPTH" "$SMT" >"$BMC_LOG" 2>&1; then
    bmc_status=PASS
  else
    bmc_status=FAIL
  fi
fi

{
  echo "# OpenTitan TLUL SVA yosys-slang run"
  echo
  echo "- OpenTitan commit: $(git -C "$OT" rev-parse HEAD)"
  echo "- Checker source: $TLUL_ASSERT_SRC"
  echo "- Filtered checker: $TLUL_ASSERT_FILTERED"
  echo "- Yosys synthesis: $synth_status"
  echo "- BMC depth: $DEPTH"
  echo "- BMC solver: $SOLVER"
  echo "- Assume legal D2H responses: $ASSUME_LEGAL_D2H"
  echo "- Immediate assume legal D2H responses: $ASSUME_LEGAL_D2H_IMMEDIATE"
  echo "- Explicit OpenTitan TLUL property clocking: $EXPLICIT_TLUL_CLOCKING"
  echo "- BMC result: $bmc_status"
  echo
  echo "## Disabled before synthesis"
  echo
  echo "| Assertion | Reason |"
  echo "| --- | --- |"
  echo '| aDataKnown_A / aDataKnown_M | use $isunknown, which current yosys-slang does not synthesize |'
  echo '| dDataKnown_A / dDataKnown_M | use $isunknown, which current yosys-slang does not synthesize |'
  echo '| aKnown_A / dKnown_A / aReadyKnown_A / dReadyKnown_A | OpenTitan knownness macros expand to $isunknown |'
  echo "| TLUL cover properties | coverage-only, and some cover sequences use unsupported match-item locals / goto repetition |"
  echo
  echo "## Synthesized assertions"
  echo
  if [ "$synth_status" = PASS ]; then
    echo "| Property | Kind |"
    echo "| --- | --- |"
    awk '/yosys-smt2-assert/ { print "| " $4 " | assert |" }
         /yosys-smt2-assume/ { print "| " $4 " | assume |" }' "$SMT"
  else
    echo "Synthesis failed; see $SYNTH_LOG."
  fi
  echo
  echo "## BMC assertions"
  echo
  if [ "$bmc_status" = PASS ]; then
    echo "| Assertion | Result |"
    echo "| --- | --- |"
    awk '/yosys-smt2-assert/ { print "| " $4 " | PASS |" }' "$SMT"
    echo
    grep -E 'Status:|Checking assertions|Temporal induction|Assert failed|BMC failed|PASSED|FAILED' "$BMC_LOG" || true
  elif [ "$bmc_status" = FAIL ]; then
    echo "| Assertion | Result |"
    echo "| --- | --- |"
    while read -r property; do
      if grep -Fq "Assert failed in tlul_sva_harness: $property" "$BMC_LOG"; then
        echo "| $property | FAIL |"
      else
        echo "| $property | PASS to depth $DEPTH |"
      fi
    done < <(awk '/yosys-smt2-assert/ { print $4 }' "$SMT")
    echo
    grep -E 'Status:|Assert failed|BMC failed|FAILED|PASSED' "$BMC_LOG" || true
    echo
    echo "See $BMC_LOG for the counterexample context."
  else
    echo "Skipped because synthesis failed."
  fi
} > "$SUMMARY"

{
  echo "tlul|$synth_status|$bmc_status|$BUILD"
  if [ "$synth_status" = FAIL ]; then
    grep -E 'ERROR:|Assert failed|Unsupported|internal error|Exception' "$SYNTH_LOG" | head -20 || true
  elif [ "$bmc_status" = FAIL ]; then
    grep -E 'Assert failed|Status:|BMC failed|FAILED' "$BMC_LOG" || true
  else
    grep -E 'Status:|PASSED' "$BMC_LOG" || true
  fi
} > "$RESULT"

echo "$SUMMARY"
exit $([ "$synth_status" = PASS ] && [ "$bmc_status" = PASS ] && echo 0 || echo 1)
