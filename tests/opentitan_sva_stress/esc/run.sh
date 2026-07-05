#!/usr/bin/env bash
# Copyright (c) 2026 The yosys-slang contributors
# SPDX-License-Identifier: ISC

set -euo pipefail

CASE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEPTH="${DEPTH:-10}"
source "$CASE_DIR/../common/common.sh"

mkdir -p "$FILTERED"
prepare_source "$OT/hw/ip/prim/fpv/vip/prim_esc_rxtx_assert_fpv.sv" "$FILTERED/prim_esc_rxtx_assert_fpv.sv"
prepare_source "$OT/hw/ip/prim/rtl/prim_esc_sender.sv" "$FILTERED/prim_esc_sender.sv"
prepare_source "$OT/hw/ip/prim/rtl/prim_esc_receiver.sv" "$FILTERED/prim_esc_receiver.sv"
prepare_source "$OT/hw/ip/prim/rtl/prim_count.sv" "$FILTERED/prim_count.sv"
prepare_source "$OT/hw/ip/prim/rtl/prim_diff_decode.sv" "$FILTERED/prim_diff_decode.sv"

run_case esc prim_esc_rxtx_sva_harness "$DEPTH" \
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
  "$CASE_DIR/harness.sv"
