// Copyright (c) 2026 The yosys-slang contributors
// SPDX-License-Identifier: ISC

module clkmgr_sva_harness (
  input logic clk_i,
  input logic rst_ni,
  input logic cg_en,
  input logic ip_clk_en,
  input logic sw_clk_en,
  input prim_mubi_pkg::mubi4_t scanmode,
  input logic gated_clk
);
  initial assume(!rst_ni);

  logic scanmode_bit;
  assign scanmode_bit = scanmode == prim_mubi_pkg::MuBi4True;

  clkmgr_cg_en_sva_if u_cg_en (
    .clk(clk_i),
    .rst_n(rst_ni),
    .ip_clk_en,
    .sw_clk_en,
    .scanmode,
    .cg_en
  );

  clkmgr_gated_clock_sva_if u_gated_clock (
    .clk(clk_i),
    .rst_n(rst_ni),
    .ip_clk_en,
    .sw_clk_en,
    .scanmode(scanmode_bit),
    .gated_clk
  );
endmodule
