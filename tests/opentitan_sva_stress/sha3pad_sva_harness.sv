// Copyright (c) 2026 The yosys-slang contributors
// SPDX-License-Identifier: ISC

module sha3pad_sva_harness
  import lc_ctrl_pkg::lc_tx_t;
(
  input logic clk_i,
  input logic rst_ni,
  input logic process_i,
  input logic keccak_complete_i,
  input logic keccak_run_o,
  input lc_tx_t lc_escalate_en_i
);
  initial assume(!rst_ni);

  sha3pad_assert_if #(
    .EnMasking(1'b1)
  ) u_assert_if (
    .clk_i,
    .rst_ni,
    .process_i,
    .keccak_complete_i,
    .keccak_run_o,
    .lc_escalate_en_i
  );
endmodule
