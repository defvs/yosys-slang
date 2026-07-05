#!/usr/bin/env bash
# Copyright (c) 2026 The yosys-slang contributors
# SPDX-License-Identifier: ISC

set -euo pipefail

CASE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEPTH="${DEPTH:-10}"
source "$CASE_DIR/../common/common.sh"

run_case fifo prim_fifo_sync_sva_harness "$DEPTH" \
  "$OT/hw/ip/prim/rtl/prim_util_pkg.sv" \
  "$OT/hw/ip/prim_generic/rtl/prim_flop.sv" \
  "$OT/hw/ip/prim/rtl/prim_count_pkg.sv" \
  "$OT/hw/ip/prim/rtl/prim_count.sv" \
  "$OT/hw/ip/prim/rtl/prim_fifo_sync_cnt.sv" \
  "$OT/hw/ip/prim/rtl/prim_fifo_sync.sv" \
  "$OT/hw/ip/prim/fpv/vip/prim_fifo_sync_assert_fpv.sv" \
  "$CASE_DIR/harness.sv"
