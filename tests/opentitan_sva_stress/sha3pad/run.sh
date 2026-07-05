#!/usr/bin/env bash
# Copyright (c) 2026 The yosys-slang contributors
# SPDX-License-Identifier: ISC

set -euo pipefail

CASE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEPTH="${DEPTH:-10}"
source "$CASE_DIR/../common/common.sh"

mkdir -p "$FILTERED"
prepare_source "$OT/hw/ip/kmac/dv/cov/sha3pad_assert_if.sv" "$FILTERED/sha3pad_assert_if.sv"

run_case sha3pad sha3pad_sva_harness "$DEPTH" \
  "$OT/hw/ip/prim/rtl/prim_util_pkg.sv" \
  "$OT/hw/ip/prim/rtl/prim_mubi_pkg.sv" \
  "$OT/hw/ip/lc_ctrl/rtl/lc_ctrl_state_pkg.sv" \
  "$OT/hw/ip/lc_ctrl/rtl/lc_ctrl_reg_pkg.sv" \
  "$OT/hw/ip/lc_ctrl/rtl/lc_ctrl_pkg.sv" \
  "$FILTERED/sha3pad_assert_if.sv" \
  "$CASE_DIR/harness.sv"
