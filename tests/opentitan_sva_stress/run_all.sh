#!/usr/bin/env bash
# Copyright (c) 2026 The yosys-slang contributors
# SPDX-License-Identifier: ISC

set -euo pipefail

STRESS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_ROOT="${BUILD_ROOT:-$STRESS_DIR/build}"
SUMMARY="$BUILD_ROOT/summary.md"

CASES=(
  fifo
  alert
  esc
  clkmgr
  sha3pad
  tlul
)

mkdir -p "$BUILD_ROOT"

status=0
for case in "${CASES[@]}"; do
  echo "== OpenTitan SVA stress: $case =="
  if ! BUILD_ROOT="$BUILD_ROOT" "$STRESS_DIR/$case/run.sh"; then
    status=1
  fi
done

{
  echo "# OpenTitan SVA stress"
  echo
  echo "The unified OpenTitan stress run executes each target-specific harness under"
  echo "\`tests/opentitan_sva_stress/<target>/run.sh\` and stores generated artifacts"
  echo "under \`$BUILD_ROOT\`."
  echo
  echo "This run checks:"
  echo
  echo "- synthesis through yosys-slang and Yosys;"
  echo "- bounded model checking through yosys-smtbmc;"
  echo "- filtered OpenTitan SVA properties, not OpenTitan \`YOSYS\` macro mode."
  echo
  echo "| Target | Synthesis | BMC | Log |"
  echo "| --- | --- | --- | --- |"
  for case in "${CASES[@]}"; do
    result="$BUILD_ROOT/$case/result.txt"
    if [ -f "$result" ]; then
      IFS='|' read -r name synth bmc dir < "$result"
      echo "| $name | $synth | $bmc | \`$dir\` |"
    else
      echo "| $case | FAIL | SKIP | missing result |"
    fi
  done
  echo
  echo "## Disabled unsupported constructs"
  echo
  echo "| Source | Item | Reason |"
  echo "| --- | --- | --- |"
  echo "| prim_alert_rxtx_assert_fpv.sv | PingEn_M | throughout and goto repetition |"
  echo "| prim_alert_rxtx_assert_fpv.sv | AlertReqAck_A / AlertCheck1_A / FsmLiveness*_A | unbounded strong eventuality |"
  echo "| prim_alert_rxtx_assert_fpv.sv | AlertPingIgnored_A / AlertCheck0_A | throughout/goto or consecutive repetition |"
  echo "| prim_esc_rxtx_assert_fpv.sv | EscDeassert_A / FsmLiveness*_A | unbounded strong eventuality and/or consecutive repetition |"
  echo "| prim_alert_rxtx_assert_fpv.sv | init_pending receiver-state helper | hierarchical enum literals triggered missing-wire frontend error; helper rewritten because remaining enabled properties do not depend on it |"
  echo "| clkmgr_cg_en_sva_if.sv | scanmode enum literal | hierarchical package enum literal rewritten to numeric MuBi4True encoding |"
  echo "| sha3pad_assert_if.sv | ProcessToRun_A / RunThenComplete_M | unbounded strong eventuality |"
  echo "| tlul_assert.sv | knownness assertions | \$isunknown is not synthesized |"
  echo "| tlul_assert.sv | legalAOpcodeErr_A / sizeGTEMaskErr_A / sizeMatchesMaskErr_A / addrSizeAlignedErr_A | unbounded s_eventually |"
  echo "| tlul_assert.sv | cover properties | coverage-only; some cover sequences use match-item locals and goto repetition |"
  echo
  echo "## Result snippets"
  echo
  for case in "${CASES[@]}"; do
    result="$BUILD_ROOT/$case/result.txt"
    echo "### $case"
    if [ -f "$result" ]; then
      tail -n +2 "$result"
    else
      echo "No result file."
    fi
    echo
  done
} > "$SUMMARY"

echo "$SUMMARY"
exit "$status"
