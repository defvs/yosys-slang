#!/usr/bin/env bash
# Copyright (c) 2026 The yosys-slang contributors
# SPDX-License-Identifier: ISC

set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TESTDIR="$ROOT/tests/opentitan_sva_stress"
OT="$ROOT/tests/third_party/opentitan"
BUILD="$TESTDIR/build"
PLUGIN="${PLUGIN:-$ROOT/build/slang.so}"
YOSYS="${YOSYS:-yosys}"
SMTBMC="${SMTBMC:-yosys-smtbmc}"
SOLVER="${SOLVER:-yices}"
DEPTH="${DEPTH:-10}"

mkdir -p "$BUILD"

prepare() {
  local src="$1"
  local dst="$2"
  python3 "$TESTDIR/prepare_stress_sources.py" --src "$src" --dst "$dst"
}

run_case() {
  local name="$1"
  local top="$2"
  shift 2
  local case_dir="$BUILD/$name"
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
  -I$BUILD/filtered \\
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

  local bmc_status=SKIP
  if [ "$synth_status" = PASS ]; then
    if "$SMTBMC" --keep-going -s "$SOLVER" -t "$DEPTH" "$smt" >"$bmc_log" 2>&1; then
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
}

FILTERED="$BUILD/filtered"
mkdir -p "$FILTERED"
prepare "$OT/hw/ip/prim/fpv/vip/prim_alert_rxtx_assert_fpv.sv" "$FILTERED/prim_alert_rxtx_assert_fpv.sv"
prepare "$OT/hw/ip/prim/fpv/vip/prim_esc_rxtx_assert_fpv.sv" "$FILTERED/prim_esc_rxtx_assert_fpv.sv"
prepare "$OT/hw/ip/prim/rtl/prim_alert_sender.sv" "$FILTERED/prim_alert_sender.sv"
prepare "$OT/hw/ip/prim/rtl/prim_alert_receiver.sv" "$FILTERED/prim_alert_receiver.sv"
prepare "$OT/hw/ip/prim/rtl/prim_esc_sender.sv" "$FILTERED/prim_esc_sender.sv"
prepare "$OT/hw/ip/prim/rtl/prim_esc_receiver.sv" "$FILTERED/prim_esc_receiver.sv"
prepare "$OT/hw/ip/prim/rtl/prim_count.sv" "$FILTERED/prim_count.sv"
prepare "$OT/hw/ip/prim/rtl/prim_diff_decode.sv" "$FILTERED/prim_diff_decode.sv"
prepare "$OT/hw/top_earlgrey/ip_autogen/clkmgr/dv/sva/clkmgr_cg_en_sva_if.sv" "$FILTERED/clkmgr_cg_en_sva_if.sv"
prepare "$OT/hw/top_earlgrey/ip_autogen/clkmgr/dv/sva/clkmgr_gated_clock_sva_if.sv" "$FILTERED/clkmgr_gated_clock_sva_if.sv"
prepare "$OT/hw/ip/kmac/dv/cov/sha3pad_assert_if.sv" "$FILTERED/sha3pad_assert_if.sv"

run_case fifo prim_fifo_sync_sva_harness \
  "$OT/hw/ip/prim/rtl/prim_util_pkg.sv" \
  "$OT/hw/ip/prim_generic/rtl/prim_flop.sv" \
  "$OT/hw/ip/prim/rtl/prim_count_pkg.sv" \
  "$OT/hw/ip/prim/rtl/prim_count.sv" \
  "$OT/hw/ip/prim/rtl/prim_fifo_sync_cnt.sv" \
  "$OT/hw/ip/prim/rtl/prim_fifo_sync.sv" \
  "$OT/hw/ip/prim/fpv/vip/prim_fifo_sync_assert_fpv.sv" \
  "$TESTDIR/prim_fifo_sync_sva_harness.sv"

run_case alert prim_alert_rxtx_sva_harness \
  "$OT/hw/ip/prim/rtl/prim_util_pkg.sv" \
  "$OT/hw/ip/prim_generic/rtl/prim_flop.sv" \
  "$OT/hw/ip/prim_generic/rtl/prim_flop_en.sv" \
  "$OT/hw/ip/prim_generic/rtl/prim_buf.sv" \
  "$OT/hw/ip/prim_generic/rtl/prim_xnor2.sv" \
  "$OT/hw/ip/prim/rtl/prim_mubi_pkg.sv" \
  "$OT/hw/ip/prim/rtl/prim_alert_pkg.sv" \
  "$OT/hw/ip/prim/rtl/prim_sec_anchor_buf.sv" \
  "$OT/hw/ip/prim/rtl/prim_sec_anchor_flop.sv" \
  "$FILTERED/prim_diff_decode.sv" \
  "$FILTERED/prim_alert_sender.sv" \
  "$FILTERED/prim_alert_receiver.sv" \
  "$OT/hw/ip/prim/fpv/tb/prim_alert_rxtx_tb.sv" \
  "$FILTERED/prim_alert_rxtx_assert_fpv.sv" \
  "$TESTDIR/prim_alert_rxtx_sva_harness.sv"

run_case esc prim_esc_rxtx_sva_harness \
  "$OT/hw/ip/prim/rtl/prim_util_pkg.sv" \
  "$OT/hw/ip/prim_generic/rtl/prim_flop.sv" \
  "$OT/hw/ip/prim_generic/rtl/prim_flop_en.sv" \
  "$OT/hw/ip/prim_generic/rtl/prim_buf.sv" \
  "$OT/hw/ip/prim_generic/rtl/prim_xnor2.sv" \
  "$OT/hw/ip/prim/rtl/prim_count_pkg.sv" \
  "$FILTERED/prim_count.sv" \
  "$OT/hw/ip/prim/rtl/prim_mubi_pkg.sv" \
  "$OT/hw/ip/prim/rtl/prim_esc_pkg.sv" \
  "$OT/hw/ip/prim/rtl/prim_sec_anchor_buf.sv" \
  "$OT/hw/ip/prim/rtl/prim_sec_anchor_flop.sv" \
  "$FILTERED/prim_diff_decode.sv" \
  "$FILTERED/prim_esc_sender.sv" \
  "$FILTERED/prim_esc_receiver.sv" \
  "$OT/hw/ip/prim/fpv/tb/prim_esc_rxtx_tb.sv" \
  "$FILTERED/prim_esc_rxtx_assert_fpv.sv" \
  "$TESTDIR/prim_esc_rxtx_sva_harness.sv"

run_case clkmgr clkmgr_sva_harness \
  "$OT/hw/ip/prim/rtl/prim_mubi_pkg.sv" \
  "$FILTERED/clkmgr_cg_en_sva_if.sv" \
  "$FILTERED/clkmgr_gated_clock_sva_if.sv" \
  "$TESTDIR/clkmgr_sva_harness.sv"

run_case sha3pad sha3pad_sva_harness \
  "$OT/hw/ip/prim/rtl/prim_util_pkg.sv" \
  "$OT/hw/ip/prim/rtl/prim_mubi_pkg.sv" \
  "$OT/hw/ip/lc_ctrl/rtl/lc_ctrl_state_pkg.sv" \
  "$OT/hw/ip/lc_ctrl/rtl/lc_ctrl_reg_pkg.sv" \
  "$OT/hw/ip/lc_ctrl/rtl/lc_ctrl_pkg.sv" \
  "$FILTERED/sha3pad_assert_if.sv" \
  "$TESTDIR/sha3pad_sva_harness.sv"

SUMMARY="$BUILD/summary.md"
{
  echo "# OpenTitan SVA stress"
  echo
  echo "| Target | Synthesis | BMC | Log |"
  echo "| --- | --- | --- | --- |"
  for result in "$BUILD"/*/result.txt; do
    IFS='|' read -r name synth bmc dir < "$result"
    echo "| $name | $synth | $bmc | \`$dir\` |"
  done
  echo
  echo "## Disabled unsupported constructs"
  echo
  echo "| Source | Item | Reason |"
  echo "| --- | --- | --- |"
  echo "| prim_alert_rxtx_assert_fpv.sv | PingEn_M | throughout and goto repetition |"
  echo "| prim_alert_rxtx_assert_fpv.sv | FullHandshake_S / PingHs_A / AlertHs_A / AlertTestHs_A | named sequence composition and/or \$changed |"
  echo "| prim_alert_rxtx_assert_fpv.sv | AlertReqAck_A / AlertCheck1_A / FsmLiveness*_A | unbounded strong eventuality |"
  echo "| prim_alert_rxtx_assert_fpv.sv | AlertPingIgnored_A / AlertCheck0_A | throughout/goto or consecutive repetition |"
  echo "| prim_esc_rxtx_assert_fpv.sv | EscDeassert_A / FsmLiveness*_A | unbounded strong eventuality and/or consecutive repetition |"
  echo "| prim_alert_rxtx_assert_fpv.sv / prim_esc_rxtx_assert_fpv.sv | sampled-value assertions using \$rose/\$fell/\$stable | current yosys-slang reports sampled-value functions as unsupported system tasks in these OpenTitan forms |"
  echo "| prim_esc_rxtx_assert_fpv.sv | SingleSigIntDetected*_A | \$onehot unsupported in current yosys-slang SVA lowering |"
  echo "| prim_alert_rxtx_assert_fpv.sv | init_pending receiver-state helper | hierarchical enum literals triggered missing-wire frontend error; helper rewritten because remaining enabled properties do not depend on it |"
  echo "| clkmgr_cg_en_sva_if.sv | CgEnOn_A / CgEnOff_A | \$fell/\$rose sampled-value functions |"
  echo "| clkmgr_gated_clock_sva_if.sv | GateOpen_A / GateClose_A | \$changed |"
  echo "| sha3pad_assert_if.sv | ProcessToRun_A / RunThenComplete_M | unbounded strong eventuality |"
  echo
  echo "## Result snippets"
  echo
  for result in "$BUILD"/*/result.txt; do
    IFS='|' read -r name synth bmc dir < "$result"
    echo "### $name"
    tail -n +2 "$result"
    echo
  done
} > "$SUMMARY"

echo "$SUMMARY"

status=0
for result in "$BUILD"/*/result.txt; do
  IFS='|' read -r _ synth bmc _dir < "$result"
  if [ "$synth" != PASS ] || [ "$bmc" != PASS ]; then
    status=1
  fi
done
exit "$status"
