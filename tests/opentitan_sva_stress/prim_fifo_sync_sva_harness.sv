// Copyright (c) 2026 The yosys-slang contributors
// SPDX-License-Identifier: ISC

module prim_fifo_sync_sva_harness (
  input logic clk_i,
  input logic rst_ni,
  input logic clr_i,
  input logic wvalid_i,
  input logic [3:0] wdata_i,
  input logic rready_i
);
  initial assume(!rst_ni);

  logic wready;
  logic rvalid;
  logic [3:0] rdata;
  logic full;
  logic [2:0] depth;
  logic err;

  prim_fifo_sync #(
    .Width(4),
    .Pass(1'b1),
    .Depth(4)
  ) u_dut (
    .clk_i,
    .rst_ni,
    .clr_i,
    .wvalid_i,
    .wready_o(wready),
    .wdata_i,
    .rvalid_o(rvalid),
    .rready_i,
    .rdata_o(rdata),
    .full_o(full),
    .depth_o(depth),
    .err_o(err)
  );

  prim_fifo_sync_assert_fpv #(
    .EnableDataCheck(1'b1),
    .Width(4),
    .Pass(1'b1),
    .Depth(4)
  ) u_assert (
    .clk_i,
    .rst_ni,
    .clr_i,
    .wvalid_i,
    .wready_o(wready),
    .wdata_i,
    .rvalid_o(rvalid),
    .rready_i,
    .rdata_o(rdata),
    .full_o(full),
    .depth_o(depth),
    .err_o(err)
  );
endmodule
