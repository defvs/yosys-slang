#!/usr/bin/env bash
# Copyright (c) 2026 The yosys-slang contributors
# SPDX-License-Identifier: ISC

set -euo pipefail

CASE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEPTH="${DEPTH:-10}"
source "$CASE_DIR/../common/common.sh"

mkdir -p "$FILTERED"
prepare_source "$OT/hw/top_earlgrey/ip_autogen/clkmgr/dv/sva/clkmgr_cg_en_sva_if.sv" "$FILTERED/clkmgr_cg_en_sva_if.sv"
prepare_source "$OT/hw/top_earlgrey/ip_autogen/clkmgr/dv/sva/clkmgr_gated_clock_sva_if.sv" "$FILTERED/clkmgr_gated_clock_sva_if.sv"

run_case clkmgr clkmgr_sva_harness "$DEPTH" \
  "$OT/hw/ip/prim/rtl/prim_mubi_pkg.sv" \
  "$FILTERED/clkmgr_cg_en_sva_if.sv" \
  "$FILTERED/clkmgr_gated_clock_sva_if.sv" \
  "$CASE_DIR/harness.sv"
