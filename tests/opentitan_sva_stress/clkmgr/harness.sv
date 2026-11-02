// Copyright (c) 2026 The sv-elab contributors
// SPDX-License-Identifier: ISC

module clkmgr_sva_harness (
  input logic clk_i,
  input logic rst_ni,
  input logic ip_clk_en,
  input logic sw_clk_en,
  input prim_mubi_pkg::mubi4_t scanmode
);
  logic first_tick = 1'b1;
  always_ff @(posedge clk_i) begin
    first_tick <= 1'b0;
    if (first_tick) assume(!rst_ni);
  end

  logic scanmode_bit;
  assign scanmode_bit = scanmode == prim_mubi_pkg::MuBi4True;

  // The checked interfaces are only collateral, stand in for the clock gate
  logic cg_en;
  logic gated_clk;
  always_ff @(posedge clk_i) begin
    cg_en <= !(ip_clk_en && sw_clk_en);
    gated_clk <= (ip_clk_en && sw_clk_en) || scanmode_bit;
  end

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
